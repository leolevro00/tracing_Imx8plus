# Function Graph Tracing

Questa sezione documenta l’attività di tracing effettuata tramite `ftrace` usando il tracer `function_graph`.

## Obiettivo

L’obiettivo era osservare cosa succede sulla CPU3 tra:

1. la scadenza del timer usato da `clock_nanosleep()`;
2. l’esecuzione del path kernel di gestione dell’`handler dell'interrupt del timer`;
3. il risveglio e la riattivazione del thread real-time `COM RTC Hnandler`

In particolare, lo scopo era capire se il jitter osservato fosse causato da un problema di scheduling, cioè da altri processi utente che venivano eseguiti prima del thread real-time, oppure se il ritardo fosse causato dall'interferenza di memoria generata dall'HMI.

## Path principale osservato

Nel trace è stato osservato un path tipico di questo tipo:

```text
arch_timer_handler_phys()
  hrtimer_interrupt()
    __hrtimer_run_queues()
      hrtimer_wakeup()
        wake_up_process()
          try_to_wake_up()
            ttwu_do_activate()
              activate_task()
              enqueue_task()
              check_preempt_curr()
