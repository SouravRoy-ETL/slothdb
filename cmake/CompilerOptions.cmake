if(MSVC)
    add_compile_options(/W4 /WX /utf-8)
    # Disable some overly noisy MSVC warnings. 4996 is the getenv/strcpy
    # "consider the _s variant" deprecation; std::getenv is standard C++
    # and is used so the GCC/Clang build stays portable.
    add_compile_options(/wd4244 /wd4267 /wd4100 /wd4996)
    # Whole-program optimization + link-time codegen for Release builds.
    # Enables cross-TU inlining of hot helpers (the radix_count_agg /
    # q21_helper / parquet inner loops are split across TUs to avoid the
    # physical_planner.cpp I-cache shift; LTCG re-inlines them across that
    # boundary so we get both stable codegen and cross-TU inlining).
    add_compile_options($<$<CONFIG:Release>:/GL>)
    add_link_options($<$<CONFIG:Release>:/LTCG>)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    add_compile_options(-Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable)
    add_compile_options(-Wno-missing-field-initializers -Wno-unused-function)
    add_compile_options(-Wno-reorder -Wno-unused-but-set-variable -Wno-maybe-uninitialized)

    if(SLOTHDB_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()
