use crate::model::{AppConfig, DISCORD_APPLICATION_ID};

#[test]
fn app_config_defaults_match_cpp_reference() {
    let config = AppConfig::default();

    assert_eq!(config.session_token(), "");
    assert_eq!(config.user_nickname(), "Nintendo Switch Player");
    assert_eq!(config.destination_folder(), "");
    assert!(!config.auto_sync());
    assert!(!config.notifications());
    assert!(!config.discord_presence());
    assert!(!config.start_on_boot());
    assert_eq!(config.sync_interval_minutes(), 60);
    assert_eq!(config.last_sync(), "Never");
    assert_eq!(config.proxy_url(), "");
    assert_eq!(config.nxapi_auth_client_id(), "eJ8TDme0c-Z4czx5SvZabA");
    assert_eq!(config.discord_application_id(), DISCORD_APPLICATION_ID);
    assert_eq!(DISCORD_APPLICATION_ID, 1_538_902_170_433_495_172);
}

#[test]
fn app_config_serialized_defaults_match_cpp_field_names_and_values() {
    let value = serde_json::to_value(AppConfig::default()).expect("serialize default config");

    assert_eq!(value["sessionToken"], "");
    assert_eq!(value["userNickname"], "Nintendo Switch Player");
    assert_eq!(value["destinationFolder"], "");
    assert_eq!(value["autoSync"], false);
    assert_eq!(value["autoSyncSettingVersion"], 1);
    assert_eq!(value["notifications"], false);
    assert_eq!(value["discordPresence"], false);
    assert_eq!(value["discordPresenceSettingVersion"], 1);
    assert_eq!(value["startOnBoot"], false);
    assert_eq!(value["syncIntervalMinutes"], 60);
    assert_eq!(value["lastSync"], "Never");
    assert_eq!(value["proxyUrl"], "");
    assert_eq!(value["nxapiAuthClientId"], "eJ8TDme0c-Z4czx5SvZabA");
    assert!(value.get("discordApplicationId").is_none());
}

#[test]
fn app_config_mutators_preserve_cpp_visible_semantics() {
    let mut config = AppConfig::default();

    config.set_session("token".to_owned(), "".to_owned());
    assert_eq!(config.session_token(), "token");
    assert_eq!(config.user_nickname(), "Nintendo Switch Player");

    config.toggle_auto_sync();
    config.toggle_notifications();
    config.toggle_discord_presence();
    config.set_start_on_boot(true);
    config.set_sync_interval_minutes(0);
    config.set_last_sync("12:34 (2026-09-05)".to_owned());
    config.set_proxy_url("  http://127.0.0.1:8080  ".to_owned());

    assert!(config.auto_sync());
    assert!(config.notifications());
    assert!(config.discord_presence());
    assert!(config.start_on_boot());
    assert_eq!(config.sync_interval_minutes(), 1);
    assert_eq!(config.last_sync(), "12:34 (2026-09-05)");
    assert_eq!(config.proxy_url(), "http://127.0.0.1:8080");

    config.clear_session();
    assert_eq!(config.session_token(), "");
}

#[test]
fn deserialization_of_zero_interval_is_normalized_at_observation_boundary() {
    let config: AppConfig = serde_json::from_value(serde_json::json!({
        "syncIntervalMinutes": 0
    }))
    .expect("deserialize config");

    assert_eq!(config.sync_interval_minutes(), 1);
}
