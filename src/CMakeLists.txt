# Each module is a static library `loki_<module>`. Dependencies must stay acyclic
# (see docs/ARCHITECTURE.md §2). Modules are added as their phase starts.
add_subdirectory(util)
add_subdirectory(model)
add_subdirectory(commands)
add_subdirectory(ui)
add_subdirectory(app)
