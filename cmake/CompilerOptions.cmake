if(MSVC)
    add_compile_options(/W4 /WX /utf-8)
    # Disable some overly noisy MSVC warnings
    add_compile_options(/wd4244 /wd4267 /wd4100)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    add_compile_options(-Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable)
    add_compile_options(-Wno-missing-field-initializers -Wno-unused-function)

    if(SLOTHDB_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()
