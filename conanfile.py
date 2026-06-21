# SPDX-License-Identifier: AGPL-3.0-or-later
from conan import ConanFile


class FreezerManagerConan(ConanFile):
    """Dependency manifest for FreezerManager.

    Kept as a Python recipe (rather than conanfile.txt) so transitive version
    conflicts can be resolved with explicit ``override=True`` — see c-ares below.
    """

    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    default_options = {
        # Drogon only needs Boost as a std::filesystem fallback on C++14; we build
        # C++20, so drop it. This avoids pulling all of Boost (including
        # boost.test, whose test_main collides with GoogleTest at link time) and
        # cuts a large source build from the dependency graph.
        "drogon/*:with_boost": False,
        # No Drogon ORM: the project uses its own IStorageBackend, not Drogon's.
        "drogon/*:with_orm": False,
    }

    def requirements(self):
        self.requires("gtest/1.17.0")
        self.requires("rapidcheck/cci.20231215")
        self.requires("spdlog/1.17.0")
        self.requires("fmt/12.1.0")
        self.requires("libsodium/1.0.21")
        self.requires("sqlite3/3.51.3")
        self.requires("libpqxx/8.0.1")
        self.requires("grpc/1.78.1")
        self.requires("protobuf/6.33.5")
        self.requires("openssl/3.6.2")
        self.requires("nlohmann_json/3.12.0")
        self.requires("abseil/20260107.1")
        self.requires("cli11/2.4.2")
        self.requires("drogon/1.9.8")
        # grpc/1.78.1 pins c-ares/1.34.6; trantor (a drogon dep) wants 1.25.0.
        # Force the newer so the graph resolves to a single c-ares.
        self.requires("c-ares/1.34.6", override=True)
        # Qt 6 (desktop client, src/qt) is intentionally NOT a Conan requirement.
        # We dynamically link the system Qt 6 (qt6-base-dev) discovered via CMake's
        # find_package(Qt6). This keeps the AGPL/commercial dual-license clean under
        # Qt's LGPLv3: dynamic linking only, no static Qt bundled. Building Qt from
        # source through Conan would also add hours to a clean build with no benefit
        # over the distro package. See src/qt/CMakeLists.txt.
