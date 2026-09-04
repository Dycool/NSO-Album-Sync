#![forbid(unsafe_code)]
#![cfg_attr(target_os = "windows", windows_subsystem = "windows")]

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.iter().any(|argument| argument == "--self-test") {
        println!("NSO Album Sync Rust self-test: safe crate initialized");
        return;
    }

    if let Err(error) = nso_album_sync::app::run(args) {
        eprintln!("NSO Album Sync: {error:#}");
        nso_album_sync::platform::show_error("NSO Album Sync", &error.to_string());
        std::process::exit(1);
    }
}
