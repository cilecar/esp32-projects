#!/bin/bash
# ================================================================
#  Склейка записей регистратора в перематываемый MP4.
#
#  Сырой .mjpeg — это просто склеенные JPEG без индекса, поэтому
#  плееры по нему не мотают. Здесь всё пересобирается в H.264 с
#  нормальным индексом: и перемотка появляется, и размер падает
#  раз в десять.
#
#  Куски объединяются в один файл по порядку номеров — запись же
#  непрерывная, резать её на десятиминутки нужно было только плате.
#
#  Использование:
#    ./convert.sh                          все записи с карты -> out.mp4
#    ./convert.sh /путь/к/файлам out.mp4   явные пути
#    FPS=8.5 ./convert.sh                  если в логе платы был не 10
#    QP=20 ./convert.sh                    качество (меньше = лучше, 18..30)
# ================================================================
set -euo pipefail

SRC="${1:-/run/media/$USER/SD}"
OUT="${2:-out.mp4}"
FPS="${FPS:-10}"
QP="${QP:-24}"
DRI="${DRI:-/dev/dri/renderD128}"

shopt -s nullglob
files=("$SRC"/cam_*.mjpeg)
shopt -u nullglob

if [ ${#files[@]} -eq 0 ]; then
    echo "В $SRC нет файлов cam_*.mjpeg" >&2
    exit 1
fi

# Имена начинаются с номера с ведущими нулями, поэтому обычная
# сортировка по алфавиту совпадает с хронологической
IFS=$'\n' files=($(sort <<<"${files[*]}")); unset IFS

total=$(du -ch "${files[@]}" | tail -1 | cut -f1)
echo "Файлов: ${#files[@]}, суммарно $total"
echo "Первый: $(basename "${files[0]}")"
echo "Последний: $(basename "${files[-1]}")"
echo "Частота: $FPS fps, качество QP=$QP"
echo

# Все куски обязаны быть одного разрешения. Если оно меняется посреди потока,
# аппаратный кодировщик спотыкается на стыке и молча обрывает запись — файл
# выглядит целым, но короче. Лучше отказаться сразу, чем отдать обрезок.
declare -A sizes
for f in "${files[@]}"; do
    wh=$(ffprobe -v error -f mjpeg -select_streams v:0 \
                 -show_entries stream=width,height -of csv=p=0 "$f" 2>/dev/null | head -1)
    sizes["${wh:-неизвестно}"]+="$(basename "$f") "
done

if [ ${#sizes[@]} -gt 1 ]; then
    echo "ОШИБКА: в архиве куски разного разрешения." >&2
    for k in "${!sizes[@]}"; do
        cnt=$(wc -w <<<"${sizes[$k]}")
        echo "  $k — $cnt файл(ов)" >&2
    done
    echo >&2
    echo "Склей их отдельными группами: разложи по разным папкам" >&2
    echo "и запусти скрипт для каждой." >&2
    exit 1
fi

echo "Разрешение: ${!sizes[*]}"
echo

# Кодировщик выбираем по наличию VAAPI: на видеокарте это секунды,
# на процессоре — минуты, но работать должно и там и там
if [ -e "$DRI" ]; then
    echo "Кодирую через VAAPI ($DRI)"
    # format=nv12|vaapi — именно с альтернативой, иначе ffmpeg не может
    # состыковать программный формат JPEG с аппаратным буфером и падает
    enc=(-vaapi_device "$DRI" -vf 'format=nv12|vaapi,hwupload' -c:v h264_vaapi -qp "$QP")
else
    echo "VAAPI недоступен, кодирую процессором"
    enc=(-c:v libx264 -preset veryfast -crf "$QP" -pix_fmt yuv420p)
fi

# Раз MJPEG — это просто поток JPEG подряд, куски можно слить
# обычным cat и скормить ffmpeg одним потоком
cat "${files[@]}" | ffmpeg -hide_banner -loglevel warning -stats \
    -f mjpeg -r "$FPS" -i - \
    "${enc[@]}" \
    -movflags +faststart \
    -y "$OUT"

echo
# Сверяем длительность с ожидаемой: это ловит обрыв, если кодировщик
# всё-таки споткнулся где-то в середине
frames=$(ffprobe -v error -show_entries stream=nb_frames -of csv=p=0 "$OUT" 2>/dev/null)
dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$OUT" 2>/dev/null)

printf "Готово: %s (%s)\n" "$OUT" "$(du -h "$OUT" | cut -f1)"
printf "Кадров: %s, длительность: %s\n" "${frames:-?}" \
       "$(printf '%dч %02dм %02dс' $((${dur%.*}/3600)) $((${dur%.*}%3600/60)) $((${dur%.*}%60)))"
