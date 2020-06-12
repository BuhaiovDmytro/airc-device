#ifndef TASKS_DEF_H
#define TASKS_DEF_H

#define INIT_TASK_PRIO                  (tskIDLE_PRIORITY)
#define INIT_TASK_STACK_SIZE            (configMINIMAL_STACK_SIZE * 2)

#define ETHIF_IN_TASK_PRIO              (tskIDLE_PRIORITY + 3)
#define ETHIF_IN_TASK_STACK_SIZE        (configMINIMAL_STACK_SIZE * 4)

#define LINK_STATE_TASK_PRIO            (tskIDLE_PRIORITY + 2)
#define LINK_STATE_TASK_STACK_SIZE      (configMINIMAL_STACK_SIZE * 2)

#define DHCP_FSM_TASK_PRIO              (tskIDLE_PRIORITY + 1)
#define DHCP_FSM_TASK_STACK_SIZE        (configMINIMAL_STACK_SIZE * 2)

#define ECHO_SERVER_TASK_PRIO           (tskIDLE_PRIORITY + 1)
#define ECHO_SERVER_TASK_STACK_SIZE     (configMINIMAL_STACK_SIZE * 2)

#define ESP8266_WIFI_TASK_PRIO          (tskIDLE_PRIORITY + 1)
#define ESP8266_WIFI_TASK_STACK_SIZE    (configMINIMAL_STACK_SIZE * 2)

#endif /* TASKS_DEF_H */

