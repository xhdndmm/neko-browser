# neko-browser source tree.
#
# Modules are built bottom-up: base is the foundation every other module
# depends on.  See docs/architecture/architecture.md for the full picture.

add_subdirectory(base)
add_subdirectory(url)
add_subdirectory(network)
add_subdirectory(dom)
add_subdirectory(html)
add_subdirectory(css)
add_subdirectory(style)
add_subdirectory(layout)
add_subdirectory(paint)
add_subdirectory(renderer)
add_subdirectory(image)
add_subdirectory(media)
add_subdirectory(pdf)
add_subdirectory(storage)
add_subdirectory(browser)
