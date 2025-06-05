idf_component_register(SRCS "ui_task.c" "net_task.c" "calendar.c" "main.c" "font_task.c"
                    INCLUDE_DIRS "include")
target_add_binary_data(${COMPONENT_TARGET} "isrgrootx1.pem" TEXT)
