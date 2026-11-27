#!/bin/sh
# Slim process supervisor for AP mode (replaces stock /fhrom/fhshell/process_check.sh).
# Drops SAF/LXC, TR-069, gdecms, dbuscheck, EasyMesh, province agents, and
# the OMCI-missing reboot trap while keeping core Wi-Fi, DHCP, and web/recovery alive.

export PATH=/fhrom/fhshell:/usr/bin:/usr/sbin:/bin:/sbin:/fhrom/bin
export LD_LIBRARY_PATH=/lib:/usr/lib:/fhrom/lib:/usr/lib/glib-2.0

wifi_check() {
	[ -f /fhdata/factorymodeflag ] && return
	if [ -z "$(pidof wifimgr)" ] && [ -x /fhrom/bin/wifimgr ]; then
		/fhrom/bin/wifimgr >/dev/null 2>&1 &
	fi
	if [ -z "$(pidof wifictl)" ] && [ -x /fhrom/bin/wifictl ]; then
		/fhrom/bin/wifictl >/dev/null 2>&1 &
	fi
}

web_check() {
	if [ -z "$(pidof thttpd)" ]; then
		killall -9 thttpd 2>/dev/null || true
		thttpd -C /fhrom/fhconf/thttpd_nginx.conf &
	fi
	if [ -z "$(pidof nginx)" ]; then
		killall -9 nginx 2>/dev/null || true
		nginx &
	fi
}

udhcpc_check() {
	for i in $(seq 1 32); do
		file="/var/${i}_udhcpc_daemon_file"
		[ -f "$file" ] || continue
		file1="${i}_udhcpc_daemon_file"
		ifname=$(uci get -c /var "$file1.daemon_exist_ifname.value" 2>/dev/null)
		[ -n "$ifname" ] || continue
		if ! ps | grep udhcpc | grep -w "$ifname" | grep -qv grep; then
			cmd=$(uci get -c /var "$file1.udhcpc_daemon_cmd.value" 2>/dev/null)
			hn=$(uci get -c /var "$file1.daemon_hostname.value" 2>/dev/null)
			if [ -n "$hn" ] && [ "$hn" != "UCI_NULL" ]; then
				$cmd -H "$hn" &
			elif [ -n "$cmd" ]; then
				$cmd &
			fi
		fi
	done
}

eventmgr_check() {
	if [ -z "$(pidof eventmgr)" ] && [ -x /fhrom/bin/eventmgr ]; then
		/fhrom/bin/eventmgr &
	fi
}

sysproxy_check() {
	if [ -z "$(pidof sysproxy)" ] && [ -x /fhrom/bin/sysproxy ]; then
		/fhrom/fhshell/sysproxy_init.sh &
	fi
}

multiwan_check() {
	if [ -z "$(pidof multiwan)" ] && [ -x /fhrom/bin/multiwan ]; then
		/fhrom/bin/multiwan &
	fi
}

igmp_check() {
	if [ -z "$(pidof onu_igmpv3)" ] && [ -x /fhrom/bin/onu_igmpv3 ]; then
		/fhrom/bin/onu_igmpv3 &
	fi
}

while :; do
	sleep 15
	wifi_check
	web_check
	udhcpc_check
	eventmgr_check
	sysproxy_check
	multiwan_check
	igmp_check
done
