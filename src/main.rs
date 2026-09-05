#![forbid(unsafe_code)]
#![cfg_attr(target_os = "windows", windows_subsystem = "windows")]

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.iter().any(|argument| argument == "--self-test") {
        match nso_album_sync::config::runtime_directory() {
            Ok(path) if path.is_dir() => {}
            Ok(_) => {
                eprintln!("Self-test failed: private runtime directory is unavailable");
                std::process::exit(2);
            }
            Err(error) => {
                eprintln!("Self-test failed: {error}");
                std::process::exit(4);
            }
        }

        let discord = nso_album_sync::discord::DiscordPresence::new(
            nso_album_sync::model::DISCORD_APPLICATION_ID,
        );
        if !discord.self_test_runtime() {
            eprintln!("Self-test failed: Discord Social SDK runtime did not load");
            std::process::exit(3);
        }
        discord.clear();
        println!("NSO Album Sync self-test passed");
        return;
    }

    // The C++ executable only consumes --self-test and Nintendo protocol
    // callbacks. Every other command-line argument is ignored. Preserve that
    // surface exactly instead of exposing Rust-only configuration switches.
    let mut app_args = Vec::with_capacity(2);
    if let Some(executable) = args.first() {
        app_args.push(executable.clone());
    }
    if let Some(callback) = args
        .iter()
        .skip(1)
        .find(|argument| nso_album_sync::auth_callback::is_nintendo_auth_callback(argument))
    {
        app_args.push(callback.clone());
    }

    if let Err(error) = nso_album_sync::app::run(app_args) {
        eprintln!("NSO Album Sync: {error:#}");
        nso_album_sync::platform::show_error("NSO Album Sync", &error.to_string());
        std::process::exit(1);
    }
}
