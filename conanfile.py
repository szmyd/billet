from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMakeToolchain, CMakeDeps, CMake, cmake_layout
from conan.tools.files import copy
from os.path import join

required_conan_version = ">=2.0"


class BilletConan(ConanFile):
    name = "billet"
    version = "0.1.0"

    homepage = "https://github.com/szmyd/billet"
    description = "Block-device benchmark tool with parametric app-shaped workload profiles"
    topics = ("benchmark", "block-device", "io_uring", "storage")
    url = "https://github.com/szmyd/billet"
    license = "Apache-2.0"

    settings = "arch", "os", "compiler", "build_type"

    options = {
        "shared": ['True', 'False'],
        "fPIC":   ['True', 'False'],
        "coverage": ['True', 'False'],
        "sanitize": ['address', 'thread', 'False'],
    }
    default_options = {
        'shared': False,
        'fPIC': True,
        'coverage': False,
        'sanitize': False,
    }
    package_type = 'application'

    exports_sources = (
        "CMakeLists.txt",
        "cmake/*",
        "include/*",
        "src/*",
        "test/*",
        "example/*",
        "LICENSE",
    )

    def _min_cppstd(self):
        return 23

    def validate(self):
        if self.settings.os != "Linux":
            raise ConanInvalidConfiguration("billet targets Linux only (io_uring, BLK* ioctls)")
        if self.settings.compiler.get_safe("cppstd"):
            check_min_cppstd(self, self._min_cppstd())

    def config_options(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")
        if self.settings.build_type == "Debug":
            if self.options.coverage and self.options.sanitize != "False":
                raise ConanInvalidConfiguration("Sanitizer does not work with Code Coverage!")
            if self.conf.get("tools.build:skip_test", default=False):
                if self.options.coverage or self.options.sanitize != "False":
                    raise ConanInvalidConfiguration("Coverage/Sanitizer requires Testing!")

    def configure(self):
        if self.settings.build_type != "Debug":
            self.options['sisl/*'].malloc_impl = 'tcmalloc'

    def build_requirements(self):
        self.test_requires("gtest/[^1.17]")

    def requirements(self):
        # sisl owns metrics + HTTP exposition + option parsing; consumed directly, never mirrored.
        self.requires("sisl/[^14.1]@oss/dev")
        self.requires("liburing/[^2.5]")
        self.requires("hdrhistogram-c/[^0.11]")
        self.requires("nlohmann_json/[^3.11]")
        self.requires("indicators/[^2.3]")

    def layout(self):
        self.folders.source = "."
        if self.options.get_safe("sanitize") and self.options.sanitize != "False":
            self.folders.build = join("build", f"Sanitized-{self.options.sanitize}")
        elif self.options.get_safe("coverage"):
            self.folders.build = join("build", "Coverage")
        else:
            self.folders.build = join("build", str(self.settings.build_type))
        self.folders.generators = join(self.folders.build, "generators")

        self.cpp.source.includedirs = ["include"]
        self.cpp.build.bindirs = ["src/cli"]

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["CMAKE_EXPORT_COMPILE_COMMANDS"] = "ON"
        tc.variables["CTEST_OUTPUT_ON_FAILURE"] = "ON"
        tc.variables["PACKAGE_VERSION"] = self.version
        tc.variables["ENABLE_TESTS"] = "ON"
        if self.conf.get("tools.build:skip_test", default=False):
            tc.variables["ENABLE_TESTS"] = "OFF"
        if self.settings.build_type == "Debug":
            if self.options.get_safe("coverage"):
                tc.variables['BUILD_COVERAGE'] = 'ON'
            elif self.options.get_safe("sanitize") and self.options.sanitize != "False":
                if self.options.sanitize == "thread":
                    tc.variables['THREAD_SANITIZER_ON'] = 'ON'
                else:
                    tc.variables['ADDRESS_SANITIZER_ON'] = 'ON'
        if self.settings.build_type != "Debug":
            tc.variables['TCMALLOC_ON'] = 'ON'
        tc.generate()

        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
        if not self.conf.get("tools.build:skip_test", default=False):
            self.run(f"ctest --test-dir '{self.build_folder}' --output-on-failure")
