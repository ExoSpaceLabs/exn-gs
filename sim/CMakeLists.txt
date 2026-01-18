add_executable(stm32_sim
        src/main.cpp
)

target_link_libraries(stm32_sim PRIVATE
        exogs_shared
        Boost::system
)

target_include_directories(stm32_sim PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../daemon/include
)

exogs_apply_warnings(stm32_sim)
