use std::env;
use std::path::{Path, PathBuf};

const SDK_VERSION: &str = "1.10.18687";

fn require(path: &Path, description: &str) {
    if !path.exists() {
        panic!("Discord Social SDK {SDK_VERSION} is missing {description}: {}", path.display());
    }
}

fn main() {
    let manifest_dir = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let repo_root = manifest_dir.parent().expect("boundary crate must be inside repository");
    let sdk_root = env::var_os("NSO_DISCORD_SOCIAL_SDK_ROOT")
        .map(PathBuf::from)
        .unwrap_or_else(|| repo_root.join("third_party/discord_social_sdk"));
    let include = sdk_root.join("include");

    require(&include.join("discordpp.h"), "include/discordpp.h");
    require(&include.join("cdiscord.h"), "include/cdiscord.h");

    let target_os = env::var("CARGO_CFG_TARGET_OS").expect("CARGO_CFG_TARGET_OS");
    let target_env = env::var("CARGO_CFG_TARGET_ENV").unwrap_or_default();

    let mut native = cc::Build::new();
    native.cpp(true);
    native.file(manifest_dir.join("src/shim.cpp"));
    native.include(&include);
    native.define("NSO_DISCORD_SOCIAL_SDK_VERSION", Some("\"1.10.18687\""));
    native.define("NSO_DISCORD_SDK_RESOURCE_ID", Some("201"));
    if target_env == "msvc" {
        native.flag_if_supported("/std:c++20");
        native.flag_if_supported("/EHsc");
        native.flag_if_supported("/utf-8");
    } else {
        native.flag_if_supported("-std=c++20");
    }
    native.compile("nso_discord_social_sdk_shim");

    match target_os.as_str() {
        "windows" => {
            let library = sdk_root.join("lib/release/discord_partner_sdk.lib");
            let runtime = sdk_root.join("bin/release/discord_partner_sdk.dll");
            require(&library, "Windows import library");
            require(&runtime, "Windows runtime");
            println!("cargo:rustc-link-search=native={}", library.parent().unwrap().display());
            println!("cargo:rustc-link-lib=dylib=discord_partner_sdk");
            println!("cargo:rustc-link-lib=dylib=delayimp");
        }
        "macos" => {
            let framework = sdk_root.join("lib/release/discord_partner_sdk.framework");
            let binary = framework.join("Versions/A/discord_partner_sdk");
            require(&binary, "macOS framework binary");
            println!("cargo:rustc-link-search=framework={}", framework.parent().unwrap().display());
            println!("cargo:rustc-link-lib=framework=discord_partner_sdk");
        }
        "linux" => {
            let library = sdk_root.join("lib/release/libdiscord_partner_sdk.so");
            require(&library, "Linux runtime");
            println!("cargo:rustc-link-search=native={}", library.parent().unwrap().display());
            println!("cargo:rustc-link-lib=dylib=discord_partner_sdk");
        }
        other => panic!("Discord Social SDK is unsupported on target OS {other}"),
    }

    println!("cargo:rerun-if-changed=src/shim.cpp");
    println!("cargo:rerun-if-env-changed=NSO_DISCORD_SOCIAL_SDK_ROOT");
}
