#![forbid(unsafe_code)]
#![cfg_attr(target_os = "windows", windows_subsystem = "windows")]

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.iter().any(|argument| argument == "--self-test") {
        match nso_album_sync::config::runtime_directory() {
            Ok(path) if path.is_dir() => {
                println!("NSO Album Sync self-test passed");
                return;
            }
            Ok(_) => {
                eprintln!("Self-test failed: private runtime directory is unavailable");
                std::process::exit(2);
            }
            Err(error) => {
                eprintln!("Self-test failed: {error}");
                std::process::exit(2);
            }
        }
    }

    if let Err(error) = nso_album_sync::app::run(args) {
        eprintln!("NSO Album Sync: {error:#}");
        nso_album_sync::platform::show_error("NSO Album Sync", &error.to_string());
        std::process::exit(1);
    }
}
