add_subdirectory(core)
add_subdirectory(storage)
add_subdirectory(auth)
add_subdirectory(kms)
add_subdirectory(audit)
add_subdirectory(rpc)
# rest before server: the freezerd target in server/ links FreezerManager::rest,
# so the alias must already exist. rest depends only on proto (from rpc) + drogon.
add_subdirectory(rest)
add_subdirectory(server)
add_subdirectory(cli)
add_subdirectory(qt)
add_subdirectory(web)
add_subdirectory(py)
