idf_component_register(SRCS "ui_task.c" "net_task.c" "main.c"
                    INCLUDE_DIRS "include")
target_add_binary_data(${COMPONENT_TARGET} "isrgrootx1.pem" TEXT)
