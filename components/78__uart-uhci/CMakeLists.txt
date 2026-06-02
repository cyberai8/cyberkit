idf_component_register(
    SRCS "src/uart_uhci.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos
)

