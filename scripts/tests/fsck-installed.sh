# e2fsck -fn of an installed disk's ext4 system partition (partition 2), in place.
#   bash scripts/tests/fsck-installed.sh [build/live/inst.img]
img=${1:-build/live/inst.img}
line=$(sfdisk -d "$img" | grep "${img##*/}2")
start=$(echo "$line" | sed -E 's/.*start= *([0-9]+).*/\1/')
e2fsck -fn "$img?offset=$((start * 512))"
