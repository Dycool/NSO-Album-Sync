#pragma once

#include <string>
#include <vector>

namespace nso {

inline const std::vector<std::vector<std::string>>& game_alias_groups() {
    static const std::vector<std::vector<std::string>> groups = {
        {
            "The Legend of Zelda Breath of the Wild",
            "The Legend of Zelda: Breath of the Wild",
            "Breath of the Wild",
            "Breath of the Wild - Nintendo Switch 2 Edition",
            "ゼルダの伝説 ブレス オブ ザ ワイルド",
            "A Lenda de Zelda Breath of the Wild",
        },
        {
            "The Legend of Zelda Tears of the Kingdom",
            "The Legend of Zelda: Tears of the Kingdom",
            "Tears of the Kingdom",
            "Tears of the Kingdom - Nintendo Switch 2 Edition",
            "ゼルダの伝説 ティアーズ オブ ザ キングダム",
            "A Lenda de Zelda Tears of the Kingdom",
        },
        {
            "The Legend of Zelda Echoes of Wisdom",
            "The Legend of Zelda: Echoes of Wisdom",
            "Echoes of Wisdom",
            "ゼルダの伝説 知恵のかりもの",
        },
        {
            "The Legend of Zelda Link's Awakening",
            "The Legend of Zelda: Link's Awakening",
            "Link's Awakening",
            "ゼルダの伝説 夢をみる島",
        },
        {
            "The Legend of Zelda Skyward Sword HD",
            "The Legend of Zelda: Skyward Sword HD",
            "Skyward Sword HD",
            "ゼルダの伝説 スカイウォードソード HD",
        },
        {
            "Hyrule Warriors Age of Calamity",
            "Hyrule Warriors: Age of Calamity",
            "Age of Calamity",
            "ゼルダ無双 厄災の黙示録",
        },
        {
            "Hyrule Warriors Definitive Edition",
            "Hyrule Warriors: Definitive Edition",
            "ゼルダ無双 ハイラルオールスターズ DX",
        },
        {
            "Super Mario Bros. Wonder",
            "Super Mario Bros Wonder",
            "スーパーマリオブラザーズ ワンダー",
        },
        {
            "Super Mario Odyssey",
            "スーパーマリオ オデッセイ",
        },
        {
            "Mario Kart 8 Deluxe",
            "Mario Kart 8 Deluxe™",
            "マリオカート8 デラックス",
            "マリオカート8DX",
        },
        {
            "New Super Mario Bros. U Deluxe",
            "New Super Mario Bros U Deluxe",
            "New スーパーマリオブラザーズ U デラックス",
        },
        {
            "Super Mario 3D World + Bowser's Fury",
            "Super Mario 3D World + Bowsers Fury",
            "スーパーマリオ 3Dワールド ＋ フューリーワールド",
        },
        {
            "Super Mario 3D All-Stars",
            "Super Mario 3D All Stars",
            "スーパーマリオ 3Dコレクション",
        },
        {
            "Super Mario Maker 2",
            "スーパーマリオメーカー 2",
        },
        {
            "Super Mario Party",
            "スーパー マリオパーティ",
        },
        {
            "Mario Party Superstars",
            "マリオパーティ スーパースターズ",
        },
        {
            "Super Mario Party Jamboree",
            "スーパー マリオパーティ ジャンボリー",
        },
        {
            "Super Mario RPG",
            "スーパーマリオRPG",
        },
        {
            "Paper Mario The Thousand-Year Door",
            "Paper Mario: The Thousand-Year Door",
            "ペーパーマリオRPG",
        },
        {
            "Paper Mario The Origami King",
            "Paper Mario: The Origami King",
            "ペーパーマリオ オリガミキング",
        },
        {
            "Princess Peach Showtime!",
            "Princess Peach: Showtime!",
            "Showtime!",
            "プリンセスピーチ Showtime!",
        },
        {
            "Mario & Sonic at the Olympic Games Tokyo 2020",
            "Mario and Sonic at the Olympic Games Tokyo 2020",
            "MARIO & SONIC NOS JOGOS OLÍMPICOS DE TÓQUIO 2020",
            "マリオ&ソニック AT 東京2020オリンピック",
        },
        {
            "Luigi's Mansion 3",
            "Luigis Mansion 3",
            "ルイージマンション3",
        },
        {
            "Luigi's Mansion 2 HD",
            "Luigis Mansion 2 HD",
            "ルイージマンション2 HD",
        },
        {
            "Captain Toad Treasure Tracker",
            "進め！キノピオ隊長",
        },
        {
            "Mario vs. Donkey Kong",
            "Mario vs Donkey Kong",
            "マリオvs.ドンキーコング",
        },
        {
            "Donkey Kong Country Tropical Freeze",
            "Donkey Kong Country: Tropical Freeze",
            "ドンキーコング トロピカルフリーズ",
        },
        {
            "Donkey Kong Country Returns HD",
            "ドンキーコング リターンズ HD",
        },
        {
            "Animal Crossing New Horizons",
            "Animal Crossing: New Horizons",
            "New Horizons",
            "New Horizons - Nintendo Switch 2 Edition",
            "Animal Crossing New Horizons - Nintendo Switch 2 Edition",
            "あつまれ どうぶつの森",
            "あつ森",
        },
        {
            "Super Smash Bros. Ultimate",
            "Super Smash Bros Ultimate",
            "大乱闘スマッシュブラザーズ SPECIAL",
            "スマブラSP",
        },
        {
            "Splatoon 2",
            "スプラトゥーン2",
        },
        {
            "Splatoon 3",
            "スプラトゥーン3",
        },
        {
            "1-2-Switch",
            "1 2 Switch",
            "ワンツースイッチ",
        },
        {
            "Pokémon Scarlet",
            "Pokemon Scarlet",
            "ポケットモンスター スカーレット",
        },
        {
            "Pokémon Violet",
            "Pokemon Violet",
            "ポケットモンスター バイオレット",
        },
        {
            "Pokémon Legends Arceus",
            "Pokemon Legends Arceus",
            "Pokémon Legends: Arceus",
            "Pokemon Legends: Arceus",
            "Pokémon LEGENDS アルセウス",
        },
        {
            "Pokémon Legends Z-A",
            "Pokemon Legends Z-A",
            "Pokémon Legends: Z-A",
            "Pokémon LEGENDS Z-A",
        },
        {
            "Pokémon Sword",
            "Pokemon Sword",
            "ポケットモンスター ソード",
        },
        {
            "Pokémon Shield",
            "Pokemon Shield",
            "ポケットモンスター シールド",
        },
        {
            "Pokémon Brilliant Diamond",
            "Pokemon Brilliant Diamond",
            "ポケットモンスター ブリリアントダイヤモンド",
        },
        {
            "Pokémon Shining Pearl",
            "Pokemon Shining Pearl",
            "ポケットモンスター シャイニングパール",
        },
        {
            "Pokémon Let's Go, Pikachu!",
            "Pokemon Let's Go Pikachu",
            "ポケットモンスター Let's Go! ピカチュウ",
        },
        {
            "Pokémon Let's Go, Eevee!",
            "Pokemon Let's Go Eevee",
            "ポケットモンスター Let's Go! イーブイ",
        },
        {
            "New Pokémon Snap",
            "New Pokemon Snap",
            "New ポケモンスナップ",
        },
        {
            "Pokémon Mystery Dungeon Rescue Team DX",
            "ポケモン不思議のダンジョン 救助隊DX",
        },
        {
            "Kirby and the Forgotten Land",
            "星のカービィ ディスカバリー",
        },
        {
            "Kirby's Return to Dream Land Deluxe",
            "星のカービィ Wii デラックス",
        },
        {
            "Kirby Star Allies",
            "星のカービィ スターアライズ",
        },
        {
            "Pikmin 4",
            "ピクミン4",
        },
        {
            "Pikmin 3 Deluxe",
            "ピクミン3 デラックス",
        },
        {
            "Pikmin 1",
            "ピクミン1",
        },
        {
            "Pikmin 2",
            "ピクミン2",
        },
        {
            "Metroid Dread",
            "メトロイド ドレッド",
        },
        {
            "Metroid Prime Remastered",
            "メトロイドプライム リマスタード",
        },
        {
            "Metroid Prime 4 Beyond",
            "Metroid Prime 4: Beyond",
            "メトロイドプライム4 ビヨンド",
        },
        {
            "Xenoblade Chronicles Definitive Edition",
            "Xenoblade Chronicles: Definitive Edition",
            "ゼノブレイド ディフィニティブ・エディション",
        },
        {
            "Xenoblade Chronicles 2",
            "ゼノブレイド2",
        },
        {
            "Xenoblade Chronicles 3",
            "ゼノブレイド3",
        },
        {
            "Fire Emblem Three Houses",
            "Fire Emblem: Three Houses",
            "ファイアーエムブレム 風花雪月",
        },
        {
            "Fire Emblem Engage",
            "ファイアーエムブレム エンゲージ",
        },
        {
            "Fire Emblem Warriors Three Hopes",
            "ファイアーエムブレム無双 風花雪月",
        },
        {
            "Nintendo Switch Sports",
            "Nintendo Switch Sports",
        },
        {
            "Ring Fit Adventure",
            "リングフィット アドベンチャー",
        },
        {
            "Clubhouse Games 51 Worldwide Classics",
            "世界のアソビ大全51",
            "51 Worldwide Games",
        },
        {
            "Nintendo Entertainment System - Nintendo Switch Online",
            "Family Computer - Nintendo Switch Online",
            "ファミリーコンピュータ Nintendo Switch Online",
            "NES - Nintendo Switch Online",
            "FC - Nintendo Switch Online",
        },
        {
            "Super Nintendo Entertainment System - Nintendo Switch Online",
            "Super Famicom - Nintendo Switch Online",
            "スーパーファミコン Nintendo Switch Online",
            "SNES - Nintendo Switch Online",
            "SFC - Nintendo Switch Online",
        },
        {
            "Nintendo 64 - Nintendo Switch Online",
            "NINTENDO 64 - Nintendo Switch Online",
            "N64 - Nintendo Switch Online",
        },
        {
            "Game Boy - Nintendo Switch Online",
            "ゲームボーイ Nintendo Switch Online",
            "GB - Nintendo Switch Online",
        },
        {
            "Game Boy Advance - Nintendo Switch Online",
            "ゲームボーイアドバンス Nintendo Switch Online",
            "GBA - Nintendo Switch Online",
        },
        {
            "SEGA Genesis - Nintendo Switch Online",
            "SEGA Mega Drive - Nintendo Switch Online",
            "セガ メガドライブ for Nintendo Switch Online",
        },
    };

    return groups;
}

}  // namespace nso
