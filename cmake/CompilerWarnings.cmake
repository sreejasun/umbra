# Strict warnings for first-party targets (third-party code is not affected).
function(umbra_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
      -Wold-style-cast -Wcast-align -Woverloaded-virtual -Wnull-dereference)
  endif()

  if(UMBRA_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endif()
endfunction()
