local projectName = "PalpediaEnhancer"

target(projectName)
    add_rules("ue4ss.mod")
    add_includedirs("include")
    add_files("src/dllmain.cpp", "src/PalpediaFilter.cpp", "src/PalpediaUi.cpp", "src/PalpediaSettings.cpp", "src/PalpediaLocalization.cpp", "src/PalpediaQuery.cpp", "src/PalpediaText.cpp", "src/PalpediaUpdate.cpp", "src/PalpediaUpdateFetch.cpp", "src/PalpediaNotice.cpp")
