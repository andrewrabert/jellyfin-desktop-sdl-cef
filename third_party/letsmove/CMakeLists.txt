set_source_files_properties(PFMoveApplication.m PROPERTIES COMPILE_FLAGS -fno-objc-arc)
add_library(letsmove PFMoveApplication.m PFMoveApplication.h)
target_link_libraries(letsmove PRIVATE "-framework AppKit" "-framework Security")
