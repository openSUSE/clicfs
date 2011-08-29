
add_executable(mkclicfs mkclicfs.cpp)
TARGET_LINK_LIBRARIES(mkclicfs lzma crypto pthread)

add_executable(clicfs clicfs.c clicfs_common.c)
TARGET_LINK_LIBRARIES(clicfs lzma fuse pthread)

add_executable(unclicfs unclicfs.c clicfs_common.c)
TARGET_LINK_LIBRARIES(unclicfs lzma)

INSTALL(TARGETS mkclicfs clicfs unclicfs DESTINATION bin)
