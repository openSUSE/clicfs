
add_executable(mkclicfs mkclicfs.cpp)
TARGET_LINK_LIBRARIES(mkclicfs lzma crypto pthread)

add_executable(clicfs clicfs.c clicfs_common.c)
TARGET_LINK_LIBRARIES(clicfs lzma fuse pthread)

add_executable(unclicfs unclicfs.c clicfs_common.c)
TARGET_LINK_LIBRARIES(unclicfs lzma)

add_executable(clicfs_fsck clicfs_check.c clicfs_common.c)
TARGET_LINK_LIBRARIES(clicfs_fsck lzma)

INSTALL(TARGETS clicfs_fsck mkclicfs clicfs unclicfs DESTINATION bin)

