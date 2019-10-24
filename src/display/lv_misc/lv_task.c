/**
 * @file lv_task.c
 * An 'lv_task'  is a void (*fp) (void* param) type function which will be called periodically.
 * A priority (5 levels + disable) can be assigned to lv_tasks.
 */

/*********************
 *      INCLUDES
 *********************/
#include <stddef.h>
#include "lv_task.h"
#include "display/lv_hal/lv_hal_tick.h"
#include "lv_gc.h"

#if defined(LV_GC_INCLUDE)
#   include LV_GC_INCLUDE
#endif /* LV_ENABLE_GC */

/*********************
 *      DEFINES
 *********************/
#define IDLE_MEAS_PERIOD    500     /*[ms]*/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static bool lv_task_exec(lv_task_t * lv_task_p);

/**********************
 *  STATIC VARIABLES
 **********************/
static volatile bool lv_task_run = false;
static uint8_t idle_last = 0;
static bool task_deleted;
static bool task_created;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Init the lv_task module
 */
void lv_task_init(void)
{
    lv_ll_init(&LV_GC_ROOT(_lv_task_ll), sizeof(lv_task_t));

    /*Initially enable the lv_task handling*/
    lv_task_enable(true);
}

/**
 * Call it  periodically to handle lv_tasks.
 */
LV_ATTRIBUTE_TASK_HANDLER void lv_task_handler(void)
{
    LV_LOG_TRACE("lv_task_handler started");

    if(lv_task_run == false)
    {
        LV_LOG_TRACE("lv_task_handler bailed early, task run false");
        return;
    }

    /*Avoid concurrent running of the task handler*/
    static bool task_handler_mutex = false;

    bool expected = false;
    bool toSet = true;
    bool alreadyTaken = __atomic_compare_exchange(&task_handler_mutex, &expected, &toSet,/* weak*/ false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);

    if (alreadyTaken) return;

    static uint32_t idle_period_start = 0;
    static uint32_t handler_start = 0;
    static uint32_t busy_time = 0;


    handler_start = lv_tick_get();

    /* Run all task from the highest to the lowest priority
     * If a lower priority task is executed check task again from the highest priority
     * but on the priority of executed tasks don't run tasks before the executed*/
    lv_task_t * task_interrupter = NULL;
    lv_task_t * next;
    bool end_flag;
    do {
        end_flag = true;
        task_deleted = false;
        task_created = false;
        LV_GC_ROOT(_lv_task_act) = lv_ll_get_head(&LV_GC_ROOT(_lv_task_ll));
        while(LV_GC_ROOT(_lv_task_act)) {
            /* The task might be deleted if it runs only once ('once = 1')
             * So get next element until the current is surely valid*/
            next = lv_ll_get_next(&LV_GC_ROOT(_lv_task_ll), LV_GC_ROOT(_lv_task_act));

            /*We reach priority of the turned off task. There is nothing more to do.*/
            if(((lv_task_t *)LV_GC_ROOT(_lv_task_act))->prio == LV_TASK_PRIO_OFF) {
                break;
            }

            /*Here is the interrupter task. Don't execute it again.*/
            if(LV_GC_ROOT(_lv_task_act) == task_interrupter) {
                task_interrupter = NULL;     /*From this point only task after the interrupter comes, so the interrupter is not interesting anymore*/
                LV_GC_ROOT(_lv_task_act) = next;
                continue;                   /*Load the next task*/
            }

            /*Just try to run the tasks with highest priority.*/
            if(((lv_task_t *)LV_GC_ROOT(_lv_task_act))->prio == LV_TASK_PRIO_HIGHEST) {
                lv_task_exec(LV_GC_ROOT(_lv_task_act));
            }
            /*Tasks with higher priority then the interrupted shall be run in every case*/
            else if(task_interrupter) {
                if(((lv_task_t *)LV_GC_ROOT(_lv_task_act))->prio > task_interrupter->prio) {
                    if(lv_task_exec(LV_GC_ROOT(_lv_task_act))) {
                        task_interrupter = LV_GC_ROOT(_lv_task_act);  /*Check all tasks again from the highest priority */
                        end_flag = false;
                        break;
                    }
                }
            }
            /* It is no interrupter task or we already reached it earlier.
             * Just run the remaining tasks*/
            else {
                if(lv_task_exec(LV_GC_ROOT(_lv_task_act))) {
                    task_interrupter = LV_GC_ROOT(_lv_task_act);  /*Check all tasks again from the highest priority */
                    end_flag = false;
                    break;
                }
            }

            if(task_deleted) break;     /*If a task was deleted then this or the next item might be corrupted*/
            if(task_created) break;     /*If a task was deleted then this or the next item might be corrupted*/

            LV_GC_ROOT(_lv_task_act) = next;         /*Load the next task*/
        }
    } while(!end_flag);

    busy_time += lv_tick_elaps(handler_start);
    uint32_t idle_period_time = lv_tick_elaps(idle_period_start);
    if(idle_period_time >= IDLE_MEAS_PERIOD) {

        idle_last = (uint32_t)((uint32_t)busy_time * 100) / IDLE_MEAS_PERIOD;   /*Calculate the busy percentage*/
        idle_last = idle_last > 100 ? 0 : 100 - idle_last;                      /*But we need idle time*/
        busy_time = 0;
        idle_period_start = lv_tick_get();


    }

    __atomic_store_n(&task_handler_mutex, false, __ATOMIC_RELAXED);

    LV_LOG_TRACE("lv_task_handler ready");
}

/**
 * Create a new lv_task
 * @param task a function which is the task itself
 * @param period call period in ms unit
 * @param prio priority of the task (LV_TASK_PRIO_OFF means the task is stopped)
 * @param param free parameter
 * @return pointer to the new task
 */
lv_task_t * lv_task_create(void (*task)(void *), uint32_t period, lv_task_prio_t prio, void * param)
{
    lv_task_t * new_lv_task = NULL;
    lv_task_t * tmp;

    /*Create task lists in order of priority from high to low*/
    tmp = lv_ll_get_head(&LV_GC_ROOT(_lv_task_ll));
    if(NULL == tmp) {                               /*First task*/
        new_lv_task = lv_ll_ins_head(&LV_GC_ROOT(_lv_task_ll));
        lv_mem_assert(new_lv_task);
        if(new_lv_task == NULL) return NULL;
    } else {
        do {
            if(tmp->prio <= prio) {
                new_lv_task = lv_ll_ins_prev(&LV_GC_ROOT(_lv_task_ll), tmp);
                lv_mem_assert(new_lv_task);
                if(new_lv_task == NULL) return NULL;
                break;
            }
            tmp = lv_ll_get_next(&LV_GC_ROOT(_lv_task_ll), tmp);
        } while(tmp != NULL);

        if(tmp == NULL) {   /*Only too high priority tasks were found*/
            new_lv_task = lv_ll_ins_tail(&LV_GC_ROOT(_lv_task_ll));
            lv_mem_assert(new_lv_task);
            if(new_lv_task == NULL) return NULL;
        }
    }

    new_lv_task->period = period;
    new_lv_task->task = task;
    new_lv_task->prio = prio;
    new_lv_task->param = param;
    new_lv_task->once = 0;
    new_lv_task->last_run = lv_tick_get();

    task_created = true;

    return new_lv_task;
}

/**
 * Delete a lv_task
 * @param lv_task_p pointer to task created by lv_task_p
 */
void lv_task_del(lv_task_t * lv_task_p)
{
    lv_ll_rem(&LV_GC_ROOT(_lv_task_ll), lv_task_p);

    lv_mem_free(lv_task_p);

    if(LV_GC_ROOT(_lv_task_act) == lv_task_p) task_deleted = true;      /*The active task was deleted*/
}

/**
 * Set new priority for a lv_task
 * @param lv_task_p pointer to a lv_task
 * @param prio the new priority
 */
void lv_task_set_prio(lv_task_t * lv_task_p, lv_task_prio_t prio)
{
    /*Find the tasks with new priority*/
    lv_task_t * i;
    LL_READ(LV_GC_ROOT(_lv_task_ll), i) {
        if(i->prio <= prio) {
            if(i != lv_task_p) lv_ll_move_before(&LV_GC_ROOT(_lv_task_ll), lv_task_p, i);
            break;
        }
    }

    /*There was no such a low priority so far then add the node to the tail*/
    if(i == NULL) {
        lv_ll_move_before(&LV_GC_ROOT(_lv_task_ll), lv_task_p, NULL);
    }


    lv_task_p->prio = prio;
}

/**
 * Set new period for a lv_task
 * @param lv_task_p pointer to a lv_task
 * @param period the new period
 */
void lv_task_set_period(lv_task_t * lv_task_p, uint32_t period)
{
    lv_task_p->period = period;
}

/**
 * Make a lv_task ready. It will not wait its period.
 * @param lv_task_p pointer to a lv_task.
 */
void lv_task_ready(lv_task_t * lv_task_p)
{
    lv_task_p->last_run = lv_tick_get() - lv_task_p->period - 1;
}

/**
 * Delete the lv_task after one call
 * @param lv_task_p pointer to a lv_task.
 */
void lv_task_once(lv_task_t * lv_task_p)
{
    lv_task_p->once = 1;
}

/**
 * Reset a lv_task.
 * It will be called the previously set period milliseconds later.
 * @param lv_task_p pointer to a lv_task.
 */
void lv_task_reset(lv_task_t * lv_task_p)
{
    lv_task_p->last_run = lv_tick_get();
}

/**
 * Enable or disable the whole lv_task handling
 * @param en: true: lv_task handling is running, false: lv_task handling is suspended
 */
void lv_task_enable(bool en)
{
    lv_task_run = en;
}

/**
 * Get idle percentage
 * @return the lv_task idle in percentage
 */
uint8_t lv_task_get_idle(void)
{
    return idle_last;
}


/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * Execute task if its the priority is appropriate
 * @param lv_task_p pointer to lv_task
 * @return true: execute, false: not executed
 */
static bool lv_task_exec(lv_task_t * lv_task_p)
{
    bool exec = false;

    /*Execute if at least 'period' time elapsed*/
    uint32_t elp = lv_tick_elaps(lv_task_p->last_run);
    if(elp >= lv_task_p->period) {
        lv_task_p->last_run = lv_tick_get();
        task_deleted = false;
        task_created = false;
        lv_task_p->task(lv_task_p->param);

        /*Delete if it was a one shot lv_task*/
        if(task_deleted == false) {			/*The task might be deleted by itself as well*/
        	if(lv_task_p->once != 0) {
        	    lv_task_del(lv_task_p);
        	}
        }
        exec = true;
    }

    return exec;
}

