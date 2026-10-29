# License of the own sounds of M.A.X.R.SI

Eleven sounds of the M.A.X.R.SI fork, made for it (29.10.2026). They are dedicated to the public domain under
**CC0 1.0 Universal**: https://creativecommons.org/publicdomain/zero/1.0/

| File | Sound | Made with |
|---|---|---|
| `vehicles/scout/wait.ogg` | Scout — idle (loop) | Stable Audio 3 Small-SFX + own synthesis |
| `vehicles/scout/start.ogg` | Scout — start | Stable Audio 3 Small-SFX |
| `vehicles/scout/drive.ogg` | Scout — driving (loop) | own synthesis over a MOSS-SoundEffect v2.0 loop |
| `vehicles/scout/stop.ogg` | Scout — stop | Stable Audio 3 Small-SFX |
| `vehicles/scout/attack.ogg` | Scout — shot | MOSS-SoundEffect v2.0 |
| `buildings/mine/running.ogg` | Mine — working (loop) | own synthesis + Stable Audio 3 Small-SFX |
| `sounds/HudButton.ogg` | button of the game panel | own synthesis |
| `sounds/ObjectMenu.ogg` | unit menu | own synthesis |
| `sounds/hit_med.ogg` | hit | Stable Audio 3 Small-SFX |
| `voices/ok1.ogg` | announcer: "Ready." | Qwen3-TTS |
| `voices/ok1_ru.ogg` | announcer, Russian: "Готов." (with `"voiceLanguage": "ru"` in `maxr.json`) | Qwen3-TTS |

| Model | Revision (Hugging Face) | License of the weights |
|---|---|---|
| Stable Audio 3 Small-SFX | `stabilityai/stable-audio-3-small-sfx` @ `05ca3857` | Stability AI Community License (the outputs are ours, III(c)(iii)) |
| MOSS-SoundEffect v2.0 | `OpenMOSS-Team/MOSS-SoundEffect-v2.0` @ `e35df4d8`; sound decoder DAC-VAE — `OpenMOSS-Team/MOVA-360p` (`audio_vae`) @ `eab4aa91` | Apache-2.0 |
| Qwen3-TTS — a voice from a text description, no voice sample | `Qwen/Qwen3-TTS-12Hz-1.7B-VoiceDesign` @ `5ecdb673`, `Qwen/Qwen3-TTS-12Hz-1.7B-Base` @ `fd4b2543` | Apache-2.0 |
| own synthesis, cutting, loops, loudness | our code (numpy, scipy) | — |

- No sound of the original M.A.X. was given to a model or mixed into these files: only numbers were measured from it
  (length, loudness, frequencies, envelopes).
- Some effects are made with Stable Audio 3 Small-SFX — Powered by Stability AI.
- The DAC-VAE of MOSS is also published by Tencent (HunyuanVideo-Foley, Tencent Hunyuan Community License: no use of
  the outputs in the EU, the UK and South Korea); we accept this risk for the two MOSS effects above.
- resinstaller (the sounds of your own copy of M.A.X.) replaces these files, except `voices/ok1_ru.ogg`.

## Лицензия своих звуков

Одиннадцать звуков форка M.A.X.R.SI, сделаны для него (29.10.2026): Скаут — ожидание, старт, езда, остановка, выстрел;
шахта — работа; кнопка панели; меню юнита; попадание; диктор «Ready.» и «Готов.». Общественное достояние —
**CC0 1.0 Universal**. Чем сделан каждый файл, модели, их ревизии и лицензии весов — в таблицах выше.

- Звуки оригинальной M.A.X. ни в модели, ни в смесь не шли: из них взяты только числа (длина, громкость, частоты,
  огибающие).
- Часть эффектов сделана Stable Audio 3 Small-SFX — Powered by Stability AI.
- DAC-VAE модели MOSS выложен и Tencent (HunyuanVideo-Foley, лицензия Tencent Hunyuan Community: выходы нельзя
  использовать в ЕС, Великобритании и Южной Корее); риск для двух эффектов MOSS (езда и выстрел Скаута) принят.
- resinstaller (звуки своей копии M.A.X.) заменяет эти файлы, кроме `voices/ok1_ru.ogg`.
