function(set_compiler_options PROJECT_NAME ENABLE_WERROR)
    target_compile_features("${PROJECT_NAME}" PUBLIC cxx_std_17)
    set_target_properties("${PROJECT_NAME}" PROPERTIES CXX_EXTENSIONS OFF)
    set_target_properties("${PROJECT_NAME}" PROPERTIES POSITION_INDEPENDENT_CODE ON)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        if(ENABLE_WERROR)
            target_compile_options("${PROJECT_NAME}" PRIVATE
                -Werror
                -Wno-error=deprecated-declarations
            )
        endif()

        # SEE: https://gcc.gnu.org/onlinedocs/gcc-6.2.0/gcc/Option-Summary.html#Option-Summary
        # for a list of all options and documentation.
        target_compile_options("${PROJECT_NAME}" PRIVATE
            -fno-strict-aliasing
            -fvisibility-inlines-hidden
            -Wall
            -Wno-delete-incomplete
            -Wno-delete-non-virtual-dtor
            -Wno-missing-braces
            -Wno-missing-field-initializers
            -Wno-parentheses
            -Wno-sign-compare
            -Wno-switch
            -Wno-type-limits
            -Wno-unused-parameter
            -Wunused-variable
            -Werror=unused-variable
            -Wunused-function
            -Werror=unused-function
            -Werror=unused-result  # Error out on unused results of functions marked with LLPC_NODISCARD
        )

        target_compile_options("${PROJECT_NAME}" PRIVATE $<$<COMPILE_LANGUAGE:CXX>:
            -fno-rtti
            # Some of the games using old versions of the tcmalloc lib are
            # crashing when allocating aligned memory. C++17 enables aligned new
            # by default, so we need to disable it to prevent those crashes.
            -fno-aligned-new
            -Wno-ignored-qualifiers
            -Wno-missing-field-initializers
            -Wno-invalid-offsetof           # offsetof within non-standard-layout type 'x' is undefined
        >)

        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options("${PROJECT_NAME}" PRIVATE
                # Output with color if in terminal: https://github.com/ninja-build/ninja/wiki/FAQ
                -fdiagnostics-color=always
                -Wno-extra
                -Wno-maybe-uninitialized
                -Wno-pedantic
            )
            if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 8.4)
                target_compile_options("${PROJECT_NAME}" PRIVATE
                    -Wno-class-memaccess
                    # TODO This should be removed once all issues are fixed
                    -Wno-format-truncation
                )
            endif()
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options("${PROJECT_NAME}" PRIVATE
                # Output with color if in terminal: https://github.com/ninja-build/ninja/wiki/FAQ
                -fcolor-diagnostics
                -Wno-covered-switch-default
                -Wno-extra-semi
                -Wno-gnu-anonymous-struct
                -Wno-nested-anon-types
            )
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        # CMAKE-TODO: These are /W4 (level 4) warnings
        target_compile_options("${PROJECT_NAME}"
            PRIVATE # Warnings in interface and src
                /wd4005 # 'DEBUG' : macro redefinition ??? Defined in toolchain ??? importedllvmincludellvm/Support/Debug.h
                /wd4018 # '<' : signed/unsigned mismatch
                /wd4100 # unreferenced formal parameter
                /wd4127 # conditional expression is constant
                /wd4141 # 'inline' : used more than once
                /wd4146 # unary minus operator applied to unsigned type, result still unsigned
                /wd4189 # local variable is initialized but not referenced
                /wd4201 # nonstandard extension used : nameless struct/union
                /wd4244 # 'X' : conversion from 'Y' to 'Z', possible loss of data
                /wd4245 # 'X' : conversion from 'Y' to 'Z', signed/unsigned mismatch
                /wd4250 # 'X': inherits 'Y' via dominance
                /wd4291 # no matching operator delete found; memory will not be freed if initialization throws an exception
                /wd4267 # 'return' : conversion from 'size_t' to 'unsigned int', possible loss of data
                /wd4389 # '==' : signed/unsigned mismatch
                /wd4505 # unreferenced local function has been removed
                /wd4510 # default constructor could not be generated
                /wd4512 # assignment operator could not be generated
                /wd4589 # ignored initialization of virtual base class
                /wd4610 # struct 'X' can never be instantiated - user defined constructor required
                /wd4624 # destructor could not be generated because a base class destructor is inaccessible or deleted
                /wd4702 # unreachable code
                /wd4706 # assignment within conditional expression
                /wd4800 # forcing value to bool 'true' or 'false' (performance warning)
                /wd6246 # Local declaration of 'S' hides declaration of the same name in outer scope
                /wd6323 # Use of arithmetic operator on Boolean type(s)
        )

        target_compile_definitions("${PROJECT_NAME}" PRIVATE _SCL_SECURE_NO_WARNINGS)
        target_compile_definitions("${PROJECT_NAME}" PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        message(FATAL_ERROR "Using unknown compiler")
    endif()
endfunction()
