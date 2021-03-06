/******************************************************************************
 * sched_arinc653.c
 *
 * An ARINC653-compatible scheduling algorithm for use in Xen.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Copyright (c) 2010, DornerWorks, Ltd. <DornerWorks.com>
 */

#include <xen/config.h>
#include <xen/lib.h>
#include <xen/sched.h>
#include <xen/sched-if.h>
#include <xen/timer.h>
#include <xen/softirq.h>
#include <xen/time.h>
#include <xen/errno.h>
#include <xen/list.h>
#include <xen/guest_access.h>
#include <public/sysctl.h>

/**************************************************************************
 * Private Macros                                                         *
 **************************************************************************/

/**
 * Default timeslice for domain 0.
 */
#define DEFAULT_TIMESLICE MILLISECS(10)

/**
 * Retrieve the idle VCPU for a given physical CPU
 */
#define IDLETASK(cpu)  (idle_vcpu[cpu])

/**
 * Return a pointer to the ARINC 653-specific scheduler data information
 * associated with the given VCPU (vc)
 */
#define AVCPU(vc) ((arinc653_vcpu_t *)(vc)->sched_priv)

/**
 * Return the global scheduler private data given the scheduler ops pointer
 */
#define SCHED_PRIV(s) ((a653sched_priv_t *)((s)->sched_data))

/**
 * Return the domain specific private data given the domain pointer
 */
#define DOM_PRIV(d) ((a653sched_domain_t *)((d) ? (d)->sched_priv : NULL))

/**************************************************************************
 * Private Type Definitions                                               *
 **************************************************************************/

/**
 * The arinc653_vcpu_t structure holds ARINC 653-scheduler-specific
 * information for all non-idle VCPUs
 */
typedef struct arinc653_vcpu_s
{
    /* vc points to Xen's struct vcpu so we can get to it from an
     * arinc653_vcpu_t pointer. */
    struct vcpu *       vc;
    /* awake holds whether the VCPU has been woken with vcpu_wake() */
    bool_t              awake;
    /* list holds the linked list information for the list this VCPU
     * is stored in */
    struct list_head    list;
} arinc653_vcpu_t;

/**
 * The sched_providers_t structure holds information about a particular
 * domain and its corresponding VCPU.
 */
typedef struct sched_providers_s
{
    /* dom_handle holds the handle ("UUID") for the domain that this
     * schedule entry refers to. */
    xen_domain_handle_t dom_handle;
    /* vcpu_id holds the VCPU number for the VCPU that this schedule
     * entry refers to. */
    int                 vcpu_id;
    /* vc holds a pointer to the Xen VCPU structure */
    struct vcpu *       vc;
} sched_providers_t;

/**
 * The sched_entry_t structure holds a single entry of the
 * ARINC 653 schedule.
 */
typedef struct sched_entry_s
{
    /* service_id holds the service identification this partical entry
     * should provide */
    int service_id;
    /* runtime holds the number of nanoseconds that the VCPU for this
     * schedule entry should be allowed to run per major frame. */
    s_time_t runtime;

    /* This variable holds the number of entries that are valid in
     * the providers[] array.
     */
    unsigned int num_providers;
    /* providers holds list of domains which provides this service */
    sched_providers_t providers[ARINC653_MAX_DOMAINS_PER_SERVICE];
} sched_entry_t;

/**
 * This structure defines data that is global to an instance of the scheduler
 */
typedef struct a653sched_priv_s
{
    /* lock for the whole pluggable scheduler, nests inside cpupool_lock */
    spinlock_t lock;

    /**
     * This array holds the active ARINC 653 schedule.
     *
     * When the system tries to start a new VCPU, this schedule is scanned
     * to look for a matching (handle, VCPU #) pair. If both the handle (UUID)
     * and VCPU number match, then the VCPU is allowed to run. Its run time
     * (per major frame) is given in the third entry of the schedule.
     */
    sched_entry_t schedule[ARINC653_MAX_SERVICES_PER_SCHEDULE];

    /**
     * This variable holds the number of entries that are valid in
     * the arinc653_schedule table.
     *
     * This is not necessarily the same as the number of domains in the
     * schedule. A domain could be listed multiple times within the schedule,
     * or a domain with multiple VCPUs could have a different
     * schedule entry for each VCPU.
     */
    unsigned int num_schedule_entries;

    /**
     * the major frame time for the ARINC 653 schedule.
     */
    s_time_t major_frame;

    /**
     * the time that the next major frame starts
     */
    s_time_t next_major_frame;

    /**
     * pointers to all Xen VCPU structures for iterating through
     */
    struct list_head vcpu_list;
} a653sched_priv_t;

typedef struct a653sched_domain_s {
    domid_t parent;
    bool primary;
    bool healthy;
} a653sched_domain_t;

/**************************************************************************
 * Helper functions                                                       *
 **************************************************************************/

/**
 * This function compares two domain handles.
 *
 * @param h1        Pointer to handle 1
 * @param h2        Pointer to handle 2
 *
 * @return          <ul>
 *                  <li> <0:  handle 1 is less than handle 2
 *                  <li>  0:  handle 1 is equal to handle 2
 *                  <li> >0:  handle 1 is greater than handle 2
 *                  </ul>
 */
static int dom_handle_cmp(const xen_domain_handle_t h1,
                          const xen_domain_handle_t h2)
{
    return memcmp(h1, h2, sizeof(xen_domain_handle_t));
}

/**
 * This function searches the vcpu list to find a VCPU that matches
 * the domain handle and VCPU ID specified.
 *
 * @param ops       Pointer to this instance of the scheduler structure
 * @param handle    Pointer to handler
 * @param vcpu_id   VCPU ID
 *
 * @return          <ul>
 *                  <li> Pointer to the matching VCPU if one is found
 *                  <li> NULL otherwise
 *                  </ul>
 */
static struct vcpu *find_vcpu(
    const struct scheduler *ops,
    xen_domain_handle_t handle,
    int vcpu_id)
{
    arinc653_vcpu_t *avcpu;

    /* loop through the vcpu_list looking for the specified VCPU */
    list_for_each_entry ( avcpu, &SCHED_PRIV(ops)->vcpu_list, list )
        if ( (dom_handle_cmp(avcpu->vc->domain->handle, handle) == 0)
             && (vcpu_id == avcpu->vc->vcpu_id) )
            return avcpu->vc;

    return NULL;
}

/**
 * This function updates the pointer to the Xen VCPU structure for each entry
 * in the ARINC 653 schedule.
 *
 * @param ops       Pointer to this instance of the scheduler structure
 * @return          <None>
 */
static void update_schedule_vcpus(const struct scheduler *ops)
{
    unsigned int i, j, n_entries = SCHED_PRIV(ops)->num_schedule_entries;

    for ( i = 0; i < n_entries; i++ )
    {
        unsigned int n_providers = SCHED_PRIV(ops)->schedule[i].num_providers;
        for ( j = 0; j < n_providers; j++ )
            SCHED_PRIV(ops)->schedule[i].providers[j].vc =
                find_vcpu(ops,
                          SCHED_PRIV(ops)->schedule[i].providers[j].dom_handle,
                          SCHED_PRIV(ops)->schedule[i].providers[j].vcpu_id);
    }
}

/**
 * This function is called by the adjust_global scheduler hook to put
 * in place a new ARINC653 schedule.
 *
 * @param ops       Pointer to this instance of the scheduler structure
 *
 * @return          <ul>
 *                  <li> 0 = success
 *                  <li> !0 = error
 *                  </ul>
 */
static int
arinc653_sched_set(
    const struct scheduler *ops,
    struct xen_sysctl_arinc653_schedule *schedule)
{
    a653sched_priv_t *sched_priv = SCHED_PRIV(ops);
    s_time_t total_runtime = 0;
    unsigned int i, j;
    unsigned long flags;
    int rc = -EINVAL;

    spin_lock_irqsave(&sched_priv->lock, flags);

    /* Check for valid major frame and number of schedule entries. */
    if ( (schedule->major_frame <= 0)
         || (schedule->num_sched_entries < 1)
         || (schedule->num_sched_entries > ARINC653_MAX_SERVICES_PER_SCHEDULE) )
        goto fail;

    for ( i = 0; i < schedule->num_sched_entries; i++ )
        if ( (schedule->sched_entries[i].num_providers < 1)
             || (schedule->sched_entries[i].num_providers > ARINC653_MAX_DOMAINS_PER_SERVICE) )
            goto fail;

    for ( i = 0; i < schedule->num_sched_entries; i++ )
    {
        /* Check for a valid run time. */
        if ( schedule->sched_entries[i].runtime <= 0 )
            goto fail;

        /* Add this entry's run time to total run time. */
        total_runtime += schedule->sched_entries[i].runtime;
    }

    /*
     * Error if the major frame is not large enough to run all entries as
     * indicated by comparing the total run time to the major frame length.
     */
    if ( total_runtime > schedule->major_frame )
        goto fail;

    /* Copy the new schedule into place. */
    sched_priv->num_schedule_entries = schedule->num_sched_entries;
    sched_priv->major_frame = schedule->major_frame;
    for ( i = 0; i < schedule->num_sched_entries; i++ )
    {
        for ( j = 0; j < schedule->sched_entries[i].num_providers; j++ )
        {
            memcpy(sched_priv->schedule[i].providers[j].dom_handle,
                    schedule->sched_entries[i].service_providers[j].dom_handle,
                    sizeof(sched_priv->schedule[i].providers[j].dom_handle));
            sched_priv->schedule[i].providers[j].vcpu_id =
                schedule->sched_entries[i].service_providers[j].vcpu_id;

        }
        sched_priv->schedule[i].service_id =
            schedule->sched_entries[i].service_id;
        sched_priv->schedule[i].runtime =
            schedule->sched_entries[i].runtime;
        sched_priv->schedule[i].num_providers =
            schedule->sched_entries[i].num_providers;
    }
    update_schedule_vcpus(ops);

    /*
     * The newly-installed schedule takes effect immediately. We do not even
     * wait for the current major frame to expire.
     *
     * Signal a new major frame to begin. The next major frame is set up by
     * the do_schedule callback function when it is next invoked.
     */
    sched_priv->next_major_frame = NOW();

    rc = 0;

 fail:
    spin_unlock_irqrestore(&sched_priv->lock, flags);
    return rc;
}

/**
 * This function is called by the adjust_global scheduler hook to read the
 * current ARINC 653 schedule
 *
 * @param ops       Pointer to this instance of the scheduler structure
 * @return          <ul>
 *                  <li> 0 = success
 *                  <li> !0 = error
 *                  </ul>
 */
static int
arinc653_sched_get(
    const struct scheduler *ops,
    struct xen_sysctl_arinc653_schedule *schedule)
{
    a653sched_priv_t *sched_priv = SCHED_PRIV(ops);
    unsigned int i, j;
    unsigned long flags;

    spin_lock_irqsave(&sched_priv->lock, flags);

    schedule->num_sched_entries = sched_priv->num_schedule_entries;
    schedule->major_frame = sched_priv->major_frame;
    for ( i = 0; i < sched_priv->num_schedule_entries; i++ )
    {
        for ( j = 0; j < sched_priv->schedule[i].num_providers; j++ )
        {
            memcpy(schedule->sched_entries[i].service_providers[j].dom_handle,
                    sched_priv->schedule[i].providers[j].dom_handle,
                    sizeof(sched_priv->schedule[i].providers[j].dom_handle));
            schedule->sched_entries[i].service_providers[j].vcpu_id =
                sched_priv->schedule[i].providers[j].vcpu_id;
        }
        schedule->sched_entries[i].runtime = sched_priv->schedule[i].runtime;
    }

    spin_unlock_irqrestore(&sched_priv->lock, flags);

    return 0;
}

/**************************************************************************
 * Scheduler callback functions                                           *
 **************************************************************************/

/**
 * This function performs initialization for an instance of the scheduler.
 *
 * @param ops       Pointer to this instance of the scheduler structure
 *
 * @return          <ul>
 *                  <li> 0 = success
 *                  <li> !0 = error
 *                  </ul>
 */
static int
a653sched_init(struct scheduler *ops)
{
    a653sched_priv_t *prv;

    prv = xzalloc(a653sched_priv_t);
    if ( prv == NULL )
        return -ENOMEM;

    ops->sched_data = prv;

    prv->next_major_frame = 0;
    spin_lock_init(&prv->lock);
    INIT_LIST_HEAD(&prv->vcpu_list);

    return 0;
}

/**
 * This function performs deinitialization for an instance of the scheduler
 *
 * @param ops       Pointer to this instance of the scheduler structure
 */
static void
a653sched_deinit(struct scheduler *ops)
{
    xfree(SCHED_PRIV(ops));
    ops->sched_data = NULL;
}

/**
 * This function inserts VCPU to scheduler list of VCPU
 *
 * @param ops       Pointer to this instance of the scheduler structure
 * @param vc        Pointer to the VCPU structure for the current domain
 */
static void
a653sched_insert_vcpu(const struct scheduler *ops, struct vcpu *vc)
{

    a653sched_priv_t *sched_priv = SCHED_PRIV(ops);
    arinc653_vcpu_t *svc = vc->sched_priv;
    unsigned int entry;
    unsigned long flags;

    spin_lock_irqsave(&sched_priv->lock, flags);

    ASSERT (!is_idle_vcpu(vc));
    /* 
     * Add every one of dom0's vcpus to the schedule, as long as there are
     * slots available.
     */
    if ( vc->domain->domain_id == 0 )
    {
        entry = sched_priv->num_schedule_entries;

        if ( entry < ARINC653_MAX_SERVICES_PER_SCHEDULE )
        {
            sched_priv->schedule[entry].providers[0].dom_handle[0] = '\0';
            sched_priv->schedule[entry].providers[0].vcpu_id = vc->vcpu_id;
            sched_priv->schedule[entry].providers[0].vc = vc;
            sched_priv->schedule[entry].runtime = DEFAULT_TIMESLICE;
            sched_priv->schedule[entry].num_providers = 1;

            sched_priv->major_frame += DEFAULT_TIMESLICE;
            ++sched_priv->num_schedule_entries;
        }
    }
    list_add(&svc->list, &SCHED_PRIV(ops)->vcpu_list);
    update_schedule_vcpus(ops);

    spin_unlock_irqrestore(&sched_priv->lock, flags);
}

/**
 * This function removes VCPU from scheduler list of VCPU
 *
 * @param ops       Pointer to this instance of the scheduler structure
 * @param vc        Pointer to the VCPU structure for the current domain
 */
static void
a653sched_remove_vcpu(const struct scheduler *ops, struct vcpu *vc)
{

    a653sched_priv_t *sched_priv = SCHED_PRIV(ops);
    arinc653_vcpu_t *svc = vc->sched_priv;
    unsigned long flags;

    spin_lock_irqsave(&sched_priv->lock, flags);

    ASSERT (!is_idle_vcpu(vc));
    list_del(&svc->list);
    update_schedule_vcpus(ops);

    spin_unlock_irqrestore(&sched_priv->lock, flags);
}

/**
 * This function allocates scheduler-specific data for a VCPU
 *
 * @param ops       Pointer to this instance of the scheduler structure
 *
 * @return          Pointer to the allocated data
 */
static void *
a653sched_alloc_vdata(const struct scheduler *ops, struct vcpu *vc, void *dd)
{
    a653sched_priv_t *sched_priv = SCHED_PRIV(ops);
    arinc653_vcpu_t *svc;
    unsigned long flags;

    /*
     * Allocate memory for the ARINC 653-specific scheduler data information
     * associated with the given VCPU (vc).
     */
    svc = xmalloc(arinc653_vcpu_t);
    if ( svc == NULL )
        return NULL;

    spin_lock_irqsave(&sched_priv->lock, flags);

    /*
     * Initialize our ARINC 653 scheduler-specific information for the VCPU.
     * The VCPU starts "asleep." When Xen is ready for the VCPU to run, it
     * will call the vcpu_wake scheduler callback function and our scheduler
     * will mark the VCPU awake.
     */
    svc->vc = vc;
    svc->awake = 0;

    spin_unlock_irqrestore(&sched_priv->lock, flags);

    return svc;
}

/**
 * This function frees scheduler-specific VCPU data
 *
 * @param ops       Pointer to this instance of the scheduler structure
 */
static void
a653sched_free_vdata(const struct scheduler *ops, void *priv)
{
    arinc653_vcpu_t *av = priv;
    xfree(av);
}

/**
 * This function allocates scheduler-specific data for a domain
 *
 * We do not actually make use of any per-domain data but the hypervisor
 * expects a non-NULL return value
 *
 * @param ops       Pointer to this instance of the scheduler structure
 *
 * @return          Pointer to the allocated data
 */
static void *
a653sched_alloc_domdata(const struct scheduler *ops, struct domain *dom)
{
    a653sched_domain_t *sdom;

    sdom = xzalloc(a653sched_domain_t);
    if ( sdom == NULL )
        return NULL;

    sdom->parent = dom->domain_id;
    sdom->primary = true;
    sdom->healthy = true;

    return sdom;
}

/**
 * This function frees scheduler-specific data for a domain
 *
 * @param ops       Pointer to this instance of the scheduler structure
 */
static void
a653sched_free_domdata(const struct scheduler *ops, void *data)
{
    xfree(data);
}

/**
 * Xen scheduler callback function to sleep a VCPU
 *
 * @param ops       Pointer to this instance of the scheduler structure
 * @param vc        Pointer to the VCPU structure for the current domain
 */
static void
a653sched_vcpu_sleep(const struct scheduler *ops, struct vcpu *vc)
{
    if ( AVCPU(vc) != NULL )
        AVCPU(vc)->awake = 0;

    /*
     * If the VCPU being put to sleep is the same one that is currently
     * running, raise a softirq to invoke the scheduler to switch domains.
     */
    if ( per_cpu(schedule_data, vc->processor).curr == vc )
        cpu_raise_softirq(vc->processor, SCHEDULE_SOFTIRQ);
}

/**
 * Xen scheduler callback function to wake up a VCPU
 *
 * @param ops       Pointer to this instance of the scheduler structure
 * @param vc        Pointer to the VCPU structure for the current domain
 */
static void
a653sched_vcpu_wake(const struct scheduler *ops, struct vcpu *vc)
{
    if ( AVCPU(vc) != NULL )
        AVCPU(vc)->awake = 1;

    cpu_raise_softirq(vc->processor, SCHEDULE_SOFTIRQ);
}

static sched_providers_t * providers_candidate(sched_entry_t *entry)
{
    int i;
    sched_providers_t * start = entry->providers;
    sched_providers_t * candidate = NULL;

    for ( i = 0; i < entry->num_providers; i++ )
    {
        struct vcpu * vc;
        a653sched_domain_t * dom;

        /* Skip if this domain is deleted */
        vc = start[i].vc;
        if ( vc == NULL )
            continue;
        dom = DOM_PRIV(vc->domain);
        if ( dom == NULL )
            continue;
        /* Skip if this domain is not healthy or its predecessor have run */
        if ( !dom->healthy )
            continue;
        candidate = &start[i];
        /* if we've got this far, break */
        break;
    }

    return candidate;
}
/**
 * Xen scheduler callback function to select a VCPU to run.
 * This is the main scheduler routine.
 *
 * @param ops       Pointer to this instance of the scheduler structure
 * @param now       Current time
 *
 * @return          Address of the VCPU structure scheduled to be run next
 *                  Amount of time to execute the returned VCPU
 *                  Flag for whether the VCPU was migrated
 */
static struct task_slice
a653sched_do_schedule(
    const struct scheduler *ops,
    s_time_t now,
    bool_t tasklet_work_scheduled)
{
    struct task_slice ret;                      /* hold the chosen domain */
    struct vcpu * new_task = NULL;
    static unsigned int sched_index = 0;
    static s_time_t next_switch_time;
    static sched_entry_t * entry;
    a653sched_priv_t * sched_priv = SCHED_PRIV(ops);
    sched_providers_t * current_provider = NULL;
    const unsigned int cpu = smp_processor_id();
    unsigned long flags;

    spin_lock_irqsave(&sched_priv->lock, flags);

    if ( sched_priv->num_schedule_entries < 1 )
        sched_priv->next_major_frame = now + DEFAULT_TIMESLICE;
    else if ( now >= sched_priv->next_major_frame )
    {
        s_time_t start_time;

        /* time to enter a new major frame
         * the first time this function is called, this will be true */
        /* start with the first domain in the schedule */
        sched_index = 0;
        entry = sched_priv->schedule;

        start_time = sched_priv->next_major_frame;
        sched_priv->next_major_frame = start_time + sched_priv->major_frame;
        next_switch_time = start_time + entry->runtime;

        current_provider = providers_candidate(entry);

    }
    else
    {
        while ( (now >= next_switch_time)
                && (sched_index < sched_priv->num_schedule_entries) )
        {
            /* time to switch to the next domain in this major frame */
            sched_index++;
            entry = sched_priv->schedule + sched_index;
            next_switch_time += entry->runtime;
        }

        /* Choose the most appropriate provider for current entry */
        current_provider = providers_candidate(entry);
    }

    if ( current_provider != NULL )
        new_task = current_provider->vc;
    /*
     * If we exhausted the domains in the schedule and still have time left
     * in the major frame then switch next at the next major frame with
     * idle vcpu.
     */
    if ( sched_index >= sched_priv->num_schedule_entries )
    {
        next_switch_time = sched_priv->next_major_frame;
        new_task = IDLETASK(cpu);
    }


    /* Check to see if the new task can be run (awake & runnable). */
    if ( !((new_task != NULL)
           && (AVCPU(new_task) != NULL)
           && AVCPU(new_task)->awake
           && vcpu_runnable(new_task)) )
        new_task = IDLETASK(cpu);
    BUG_ON(new_task == NULL);

    /*
     * Check to make sure we did not miss a major frame.
     * This is a good test for robust partitioning.
     */
    BUG_ON(now >= sched_priv->next_major_frame);

    spin_unlock_irqrestore(&sched_priv->lock, flags);

    /* Tasklet work (which runs in idle VCPU context) overrides all else. */
    if ( tasklet_work_scheduled )
        new_task = IDLETASK(cpu);

    /* Running this task would result in a migration */
    if ( !is_idle_vcpu(new_task)
         && (new_task->processor != cpu) )
        new_task = IDLETASK(cpu);

    /*
     * Return the amount of time the next domain has to run and the address
     * of the selected task's VCPU structure.
     */
    ret.time = next_switch_time - now;
    ret.task = new_task;
    ret.migrated = 0;

    BUG_ON(ret.time <= 0);

    return ret;
}

/**
 * Xen scheduler callback function to select a CPU for the VCPU to run on
 *
 * @param ops       Pointer to this instance of the scheduler structure
 * @param v         Pointer to the VCPU structure for the current domain
 *
 * @return          Number of selected physical CPU
 */
static int
a653sched_pick_cpu(const struct scheduler *ops, struct vcpu *vc)
{
    cpumask_t *online;
    unsigned int cpu;

    /* 
     * If present, prefer vc's current processor, else
     * just find the first valid vcpu .
     */
    online = cpupool_domain_cpumask(vc->domain);

    cpu = cpumask_first(online);

    if ( cpumask_test_cpu(vc->processor, online)
         || (cpu >= nr_cpu_ids) )
        cpu = vc->processor;

    return cpu;
}

/**
 * Xen scheduler callback to change the scheduler of a cpu
 *
 * @param new_ops   Pointer to this instance of the scheduler structure
 * @param cpu       The cpu that is changing scheduler
 * @param pdata     scheduler specific PCPU data (we don't have any)
 * @param vdata     scheduler specific VCPU data of the idle vcpu
 */
static void
a653_switch_sched(struct scheduler *new_ops, unsigned int cpu,
                  void *pdata, void *vdata)
{
    struct schedule_data *sd = &per_cpu(schedule_data, cpu);
    arinc653_vcpu_t *svc = vdata;

    ASSERT(!pdata && svc && is_idle_vcpu(svc->vc));

    idle_vcpu[cpu]->sched_priv = vdata;

    per_cpu(scheduler, cpu) = new_ops;
    per_cpu(schedule_data, cpu).sched_priv = NULL; /* no pdata */

    /*
     * (Re?)route the lock to its default location. We actually do not use
     * it, but if we leave it pointing to where it does now (i.e., the
     * runqueue lock for this PCPU in the default scheduler), we'd be
     * causing unnecessary contention on that lock (in cases where it is
     * shared among multiple PCPUs, like in Credit2 and RTDS).
     */
    sd->schedule_lock = &sd->_lock;
}

/**
 * Xen scheduler callback function to perform a global (not domain-specific)
 * adjustment. It is used by the ARINC 653 scheduler to put in place a new
 * ARINC 653 schedule or to retrieve the schedule currently in place.
 *
 * @param ops       Pointer to this instance of the scheduler structure
 * @param sc        Pointer to the scheduler operation specified by Domain 0
 */
static int
a653sched_adjust_global(const struct scheduler *ops,
                        struct xen_sysctl_scheduler_op *sc)
{
    xen_sysctl_arinc653_schedule_t local_sched;
    int rc = -EINVAL;

    switch ( sc->cmd )
    {
    case XEN_SYSCTL_SCHEDOP_putinfo:
        if ( copy_from_guest(&local_sched, sc->u.sched_arinc653.schedule, 1) )
        {
            rc = -EFAULT;
            break;
        }

        rc = arinc653_sched_set(ops, &local_sched);
        break;
    case XEN_SYSCTL_SCHEDOP_getinfo:
        memset(&local_sched, -1, sizeof(local_sched));
        rc = arinc653_sched_get(ops, &local_sched);
        if ( rc )
            break;

        if ( copy_to_guest(sc->u.sched_arinc653.schedule, &local_sched, 1) )
            rc = -EFAULT;
        break;
    }

    return rc;
}

static int
a653sched_init_domain(const struct scheduler *ops,
                     struct domain *dom)
{
    a653sched_domain_t *sdom;

    printk("New domain [%d]\n", dom->domain_id);
    sdom = a653sched_alloc_domdata(ops, dom);

    if ( sdom == NULL )
        return -ENOMEM;

    dom->sched_priv = sdom;

    return 0;
}

static void
a653sched_destroy_domain(const struct scheduler *ops, struct domain *dom)
{
    a653sched_free_domdata(ops, dom->sched_priv);

    dom->sched_priv = NULL;
}

static int
a653sched_adjust_domain(const struct scheduler *ops, struct domain *d,
                        struct xen_domctl_scheduler_op *op)
{
    a653sched_priv_t * const sched_priv = SCHED_PRIV(ops);
    a653sched_domain_t * const sdom = DOM_PRIV(d);
    unsigned long flags;

    spin_lock_irqsave(&sched_priv->lock, flags);

    switch (op->cmd) {
    case XEN_DOMCTL_SCHEDOP_putinfo:
        printk("dom [%d]: parent = %d -> %d | primary = %s\n",
                d->domain_id, sdom->parent, op->u.arinc653.parent,
                (sdom->primary) ? "primary" : "backup");
        if (op->u.arinc653.parent != -1)
        {
            sdom->parent = op->u.arinc653.parent;
            sdom->primary = (sdom->parent == d->domain_id);
        }
        sdom->healthy = op->u.arinc653.healthy;
        printk("health argument for domain[%d]: %d\n", d->domain_id, op->u.arinc653.healthy);
        printk("health for domain[%d]: %s\n", d->domain_id, (sdom->healthy) ? "healthy" : "not healthy");
        break;
    case XEN_DOMCTL_SCHEDOP_getinfo:
        op->u.arinc653.parent = sdom->parent;
        op->u.arinc653.healthy = sdom->healthy;
        break;
    }

    spin_unlock_irqrestore(&sched_priv->lock, flags);

    return 0;
}
/**
 * This structure defines our scheduler for Xen.
 * The entries tell Xen where to find our scheduler-specific
 * callback functions.
 * The symbol must be visible to the rest of Xen at link time.
 */
static const struct scheduler sched_arinc653_def = {
    .name           = "ARINC 653 Scheduler",
    .opt_name       = "arinc653",
    .sched_id       = XEN_SCHEDULER_ARINC653,
    .sched_data     = NULL,

    .init           = a653sched_init,
    .deinit         = a653sched_deinit,

    .free_vdata     = a653sched_free_vdata,
    .alloc_vdata    = a653sched_alloc_vdata,

    .free_domdata   = a653sched_free_domdata,
    .alloc_domdata  = a653sched_alloc_domdata,

    .init_domain    = a653sched_init_domain,
    .destroy_domain = a653sched_destroy_domain,

    .insert_vcpu    = a653sched_insert_vcpu,
    .remove_vcpu    = a653sched_remove_vcpu,

    .sleep          = a653sched_vcpu_sleep,
    .wake           = a653sched_vcpu_wake,
    .yield          = NULL,
    .context_saved  = NULL,

    .do_schedule    = a653sched_do_schedule,

    .pick_cpu       = a653sched_pick_cpu,

    .switch_sched   = a653_switch_sched,

    .adjust         = a653sched_adjust_domain,
    .adjust_global  = a653sched_adjust_global,

    .dump_settings  = NULL,
    .dump_cpu_state = NULL,

    .tick_suspend   = NULL,
    .tick_resume    = NULL,
};

REGISTER_SCHEDULER(sched_arinc653_def);

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
