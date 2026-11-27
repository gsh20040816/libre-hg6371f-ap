package main

import (
	"fmt"
	"strconv"
	"strings"
)

func formatFCStatus(hwAccel, flowLearn bool, ucastMax, ucastCreated, ucastEvicted uint32) string {
	hwStr := "Disabled"
	if hwAccel {
		hwStr = "Enabled"
	}
	flStr := "Disabled"
	if flowLearn {
		flStr = "Enabled"
	}
	active := ucastCreated - ucastEvicted
	return fmt.Sprintf(
		"\tHW Acceleration <%s> \n\tFlow Ucast Learning %s  : Max<%d>, Active<%d>, Cumulative [ %d - %d ]\n",
		hwStr, flStr, ucastMax, active, ucastCreated, ucastEvicted,
	)
}

func bootstateCharName(ch int) string {
	switch ch {
	case '0':
		return "BOOT_SET_NEW_IMAGE"
	case '1':
		return "BOOT_SET_OLD_IMAGE"
	case '2':
		return "BOOT_SET_NEW_IMAGE_ONCE"
	case 'A':
		return "BOOT_SET_OLD_IMAGE_ONCE"
	case '5':
		return "BOOT_SET_PART1_IMAGE"
	case '7':
		return "BOOT_SET_PART1_IMAGE_ONCE"
	case '6':
		return "BOOT_SET_PART2_IMAGE"
	case '8':
		return "BOOT_SET_PART2_IMAGE_ONCE"
	default:
		return "UNKNOWN (boot latest image)"
	}
}

func formatBootstateText(booted, reboot, stateChar, committed int, valid [2]int, seq [2]int, tags [2]string) string {
	partName := func(p int) string {
		if p == 1 {
			return "First"
		}
		return "Second"
	}
	var sb strings.Builder
	fmt.Fprintf(&sb, "rd_metadata: committed %d valid %d,%d seq %d,%d\n",
		committed, valid[0], valid[1], seq[0], seq[1])
	fmt.Fprintf(&sb, "\n  Boot image state: %s\n", bootstateCharName(stateChar))
	fmt.Fprintf(&sb, "  Booted Partition: %s\n", partName(booted))
	fmt.Fprintf(&sb, "  Reboot Partition: %s\n\n", partName(reboot))

	for p := 1; p <= 2; p++ {
		b := ' '
		if booted == p {
			b = 'B'
		}
		r := ' '
		if reboot == p {
			r = '>'
		}
		label := "First "
		if p == 2 {
			label = "Second"
		}
		sq := seq[p-1]
		typ := "pureUBI/GPT"
		if sq < 0 {
			typ = "NONE"
		}
		cflag := "0"
		if committed == p {
			cflag = "1"
		}
		fmt.Fprintf(&sb, "%c%c%s partition type           : %s\n", b, r, label, typ)
		fmt.Fprintf(&sb, "%c%c%s partition sequence number: %d\n", b, r, label, sq)
		fmt.Fprintf(&sb, "%c%c%s partition image tag      : %s\n", b, r, label, tags[p-1])
		fmt.Fprintf(&sb, "%c%c%s partition commit flag    : %s\n", b, r, label, cflag)
	}
	return sb.String()
}

func parseBootstateCode(s string) (int, bool) {
	switch s {
	case "1", "BOOT_SET_NEW_IMAGE":
		return 1, true
	case "2", "BOOT_SET_OLD_IMAGE":
		return 2, true
	case "3", "BOOT_SET_NEW_IMAGE_ONCE":
		return 3, true
	case "4", "BOOT_SET_OLD_IMAGE_ONCE":
		return 4, true
	case "5", "BOOT_SET_PART1_IMAGE":
		return 5, true
	case "6", "BOOT_SET_PART1_IMAGE_ONCE":
		return 6, true
	case "7", "BOOT_SET_PART2_IMAGE":
		return 7, true
	case "8", "BOOT_SET_PART2_IMAGE_ONCE":
		return 8, true
	default:
		return 0, false
	}
}

func formatWlSpect(val int32) (string, bool) {
	switch val {
	case 0:
		return "Off\n", true
	case 1:
		return "Loose interpretation of 11h spec - may join non 11h AP.\n", true
	case 2:
		return "Strict interpretation of 11h spec - may not join non 11h AP.\n", true
	case 3:
		return "802.11d mode\n", true
	case 4:
		return "Loose interpretation of 11h+d spec - may join non-11h APs\n", true
	default:
		return "", false
	}
}

func formatWlVarInt(val int32) string {
	if val <= 9 {
		return fmt.Sprintf("%d\n", val)
	}
	return fmt.Sprintf("%d (0x%x)\n", val, uint32(val))
}

func parseWlBand(s string) (uint32, bool) {
	switch strings.ToLower(s) {
	case "a", "5", "5g":
		return 1, true
	case "b", "2", "2g":
		return 2, true
	case "6", "6g":
		return 4, true
	default:
		return 0, false
	}
}

func parseWlIntArg(s string) (int32, bool) {
	v, err := strconv.ParseInt(strings.TrimSpace(s), 0, 32)
	if err != nil {
		return 0, false
	}
	return int32(v), true
}

func parseWlUintArg(s string) (uint32, bool) {
	v, err := strconv.ParseUint(strings.TrimSpace(s), 0, 32)
	if err != nil {
		return 0, false
	}
	return uint32(v), true
}
