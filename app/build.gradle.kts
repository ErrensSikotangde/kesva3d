plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "org.errens.kesva3d"
    compileSdk = 37
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "org.errens.kesva3d"
        minSdk = 28
        targetSdk = 37
        versionCode = 1
        versionName = "Kesva 3D alpha-0.0.1"
        
        externalNativeBuild {
            cmake {
                // 强制 C++23 标准，开启 RTTI 与异常支持
                cppFlags += "-std=c++23"
                cppFlags += "-frtti"
                cppFlags += "-fexceptions"

                // 强制动态链接 libc++，与 bgfx 编译配置保持一致，避免 ABI 冲突
                arguments += "-DANDROID_STL=c++_shared"
            }
        }
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    
    buildFeatures {
        viewBinding = true
        prefab = true
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
}

// 构建后复制 APK 到原目录 (用于 proot 内部构建)
tasks.register<Copy>("copyApkToOriginal") {
    val originalDir = System.getenv("ORIGINAL_PROJECT_DIR")
    if (originalDir != null) {
        from(layout.buildDirectory.dir("outputs/apk"))
        into(File(originalDir, "app/build/outputs/apk"))
        doLast {
            println(">>> APK 已复制到: $originalDir/app/build/outputs/apk")
        }
    }
}

tasks.matching { it.name.startsWith("assemble") }.configureEach {
    finalizedBy("copyApkToOriginal")
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(files("libs/SDL2-2.32.10.aar"))
}