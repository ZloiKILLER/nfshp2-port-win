# Языки

Игра умеет шесть языков: английский, французский, немецкий, итальянский, испанский и шведский (`e_Languages` 0..5, ещё 6 `LONGEST` и 7 `ID`
служебные). Американский диск их все содержит: строки `LANGUAGES\FE_<ЯЗЫК>.BIN` (в `data\` это `.lang`), речь
`SOUND\SPEECH\<ЯЗЫК>BAN.PCK` и `DAT.PCK` (шведской нет), французские текстуры фронтенда (`fr_frontend`, логотип «Poursuite infernale»).
NTSC просто прятал пункт Languages в Options (`MENUITEM_7`).

## Как переключается

`LanguageManager::SetLanguage(&TheLanguageManager, язык)`: `0x21e0d0`, объект `0x31c1c8` (`CurrLanguage` на +0). Выгружает старый файл
строк, грузит `LanguageFilenames[язык]` (`0x31c168`, `char *[8]`), затем `HandleLanguageChange`: речь, `GameFlowNotifyLanguageChange`
(подмена текстур для французского) и `FlailerUpdateLanguageText`. Работает на ходу, в любом экране фронтенда.

Порт: Options, Languages (`pcsettings.cpp`, страница `PC Languages`), `pc_language_choose` в `pclang.cpp`. Язык хранится в `hp2.ini`
(`language=FRENCH`) и ставится при первом кадре фронтенда (`pc_language_apply`).

`LanguageDescription +72` — это `e_Languages` загруженного файла. `patch_language` (`pcplatform.cpp`) при каждой загрузке кладёт в пул
строки порта для этого языка: английские из `kText`, плюс строки `#id` из файла языка.

`FlailerTextChunk::SetString` сам ищет английскую строку в хеш-таблице языка: если игра знает строку («Audio»), на экране будет её перевод
(«Sonido»). Строки, которых игра не знает, остаются как переданы.

## Строки порта

`data\lang\<ЯЗЫК>.txt`, UTF-8, формат в `recomp/runtime/pclang.h`. Пишет их `tools/recomp/make_lang.py` из одной таблицы на все языки.
`tr(english)` в `pcmenu.cpp` (через `set_text` и `blurb_text`) отдаёт перевод по английскому тексту. `set_text` запоминает английский текст
каждого объекта, и смена языка ставит их заново (`pc_retranslate`). Место под текст: подписи Options и страниц 32 символа, подсказка Back
16. Длинная подсказка внизу прокручивается сама.

Надпись «SAVED» в углу рисуется своим шрифтом 5x7 (`draw_notice`, `native_vk.cpp`): A-Z, акценты снимаются.

## Свой язык (например, русский)

1. Файл `data\lang\RUSSIAN.txt`: `@name`, `@slot 6` (место `LONGEST`; `7` — место `ID`), `@file LANGUAGES\FE_RUSSIAN.BIN`, и переводы
   строк порта (копия любого файла). Порт поставит `LanguageFilenames[6]` на этот файл.
2. Сам `FE_RUSSIAN.BIN`: чанк `0x39000` в формате `data-files.md` (ID-таблица, хеш-таблица по английскому тексту, пул текста). Инструмента
   сборки пока нет: проще всего взять `FE_ENGLISH.lang` и заменить тексты. Файл должен быть в `data\` и в `data\index.txt` (по хешу имени,
   как пишет `tools/recomp/unpack_data.py`); `disc.cpp` собирает диск только из `index.txt`.
3. Шрифты. Тексты идут в игру в Latin-1, и в шрифтах игры только эти буквы. Для кириллицы нужны свои глифы: текстуры шрифтов `FONT_*`
   через `data\replace` и их таблицы символов. Это отдельная работа.
4. Речь: без своего банка игра оставит английскую.
