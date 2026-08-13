# neko-browser source tree.
#
# Modules are built bottom-up: base is the foundation every other module
# depends on.  See docs/architecture/architecture.md for the full picture.

add_subdirectory(base)
add_subdirectory(browser)
