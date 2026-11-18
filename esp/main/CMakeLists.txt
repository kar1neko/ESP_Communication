if(CONFIG_DEVICE_ROLE_SENDER)
  set(srcs "sending_side.cpp")
else()
  set(srcs "receiving_side.cpp")
endif()

idf_component_register(SRCS ${srcs}
                        INCLUDE_DIRS ".")