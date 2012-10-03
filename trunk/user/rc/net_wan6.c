/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <net/route.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include <nvram/bcmnvram.h>
#include <netconf.h>
#include <shutils.h>
#include <signal.h>

#include "rc.h"

#if defined (USE_IPV6)

void build_dns6_var(void)
{
	char dns6s[INET6_ADDRSTRLEN*3+2] = {0};
	char *dnsnv[3];

	if (is_wan_dns6_static() == 1) {
		dnsnv[0] = nvram_safe_get("ip6_dns1");
		dnsnv[1] = nvram_safe_get("ip6_dns2");
		dnsnv[2] = nvram_safe_get("ip6_dns3");
		if (*dnsnv[0] || *dnsnv[1] || *dnsnv[2]) {
			snprintf(dns6s, sizeof(dns6s), "%s %s %s", dnsnv[0], dnsnv[1], dnsnv[2]);
			trim_r(dns6s);
		}
	}

	nvram_set("wan0_dns6", dns6s);
}

void reset_wan6_vars(void)
{
	struct in6_addr addr6;
	char addr6s[INET6_ADDRSTRLEN] = {0};
	char *wan_addr6, *wan_gate6;
	int wan_size6;

	wan_gate6 = "";
	if (is_wan_addr6_static() == 1) {
		wan_addr6 = nvram_safe_get("ip6_wan_addr");
		wan_size6 = nvram_get_int("ip6_wan_size");
		wan_gate6 = nvram_safe_get("ip6_wan_gate");
		if (*wan_addr6) {
			memset(&addr6, 0, sizeof(addr6));
			ipv6_from_string(wan_addr6, &addr6);
			inet_ntop(AF_INET6, &addr6, addr6s, INET6_ADDRSTRLEN);
			if (wan_size6 > 0 && wan_size6 < 128)
				sprintf(addr6s, "%s/%d", addr6s, wan_size6);
		}
	}

	nvram_set("wan0_addr6", addr6s);
	nvram_set("wan0_gate6", wan_gate6);
	nvram_set("wan0_6rd_relay", nvram_safe_get("ip6_6rd_relay"));
	nvram_set("wan0_6rd_size", nvram_safe_get("ip6_6rd_size"));

	build_dns6_var();
}

int is_wan_addr6_static(void)
{
	int ipv6_type = get_ipv6_type();

	if (ipv6_type == IPV6_DISABLED)
		return -1;

	if (ipv6_type == IPV6_NATIVE_DHCP6 ||
	    ipv6_type == IPV6_6TO4)
		return 0;

	if (nvram_match("ip6_wan_dhcp", "0") || // assume 6RD static
	    ipv6_type == IPV6_NATIVE_STATIC || 
	    ipv6_type == IPV6_6IN4)
		return 1;
	
	return 0;
}

int is_wan_dns6_static(void)
{
	int ipv6_type = get_ipv6_type();

	if (ipv6_type == IPV6_DISABLED)
		return -1;

	if (nvram_match("ip6_dns_auto", "0") || 
	    ipv6_type == IPV6_NATIVE_STATIC || 
	    ipv6_type == IPV6_6IN4 ||
	    ipv6_type == IPV6_6TO4 ||
	    ipv6_type == IPV6_6RD)
		return 1;
	
	return 0;
}

int is_wan_ipv6_type_sit(void)
{
	int ipv6_type = get_ipv6_type();

	if (ipv6_type == IPV6_DISABLED)
		return -1;

	if (ipv6_type == IPV6_6IN4 ||
	    ipv6_type == IPV6_6TO4 ||
	    ipv6_type == IPV6_6RD)
		return 1;
	
	return 0;
}

int is_wan_ipv6_if_ppp(void)
{
	if (nvram_get_int("ip6_wan_if") == 0)
		return 1;
	
	return 0;
}

void store_ip6rd_from_dhcp(const char *env_value, const char *prefix)
{
	int i;
	char tmp[100];
	char ip6rd[64];
	char addr6[INET6_ADDRSTRLEN];
	char *value, *values[4];

	// "32 128 FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF 255.255.255.255 "

	if (nvram_match("ip6_wan_dhcp", "0"))
		return;

	strncpy(ip6rd, env_value, sizeof(ip6rd));
	value = ip6rd;
	for (i = 0; i < 4 && value; i++)
		values[i] = strsep(&value, " ");
	if (i == 4)
	{
		snprintf(addr6, sizeof(addr6), "%s/%s", values[2], values[1]);
		nvram_set(strcat_r(prefix, "ip6_wan_addr", tmp), addr6);
		nvram_set(strcat_r(prefix, "6rd_size", tmp), values[0]);
		nvram_set(strcat_r(prefix, "6rd_relay", tmp), values[3]);
	}
}

void start_sit_tunnel(int ipv6_type, char *wan_addr4, char *wan_addr6)
{
	int sit_ttl, sit_mtu, size4, size6, lan_size6;
	char *sit_remote, *sit_relay, *wan_gate6;
	char addr6s[INET6_ADDRSTRLEN], sit_6rd_prefix[INET6_ADDRSTRLEN], sit_6rd_relay_prefix[32];
	struct in_addr addr4, net4;
	struct in6_addr addr6, net6;

	size4 = 0;
	addr4.s_addr = inet_addr_(wan_addr4);
	if (addr4.s_addr == INADDR_ANY)
		return; // cannot start SIT tunnel w/o IPv4 WAN addr

	sit_mtu = nvram_get_int("ip6_sit_mtu");
	sit_ttl = nvram_get_int("ip6_sit_ttl");
	if (sit_mtu < 1280) sit_mtu = 1280;
	if (sit_ttl < 1) sit_ttl = 1;
	if (sit_ttl > 255) sit_ttl = 255;

	memset(&addr6, 0, sizeof(addr6));
	size6 = ipv6_from_string(wan_addr6, &addr6);
	if (size6 < 0) size6 = 0;

	sit_relay = "";
	sit_remote = "any";
	if (ipv6_type == IPV6_6IN4)
		sit_remote = nvram_safe_get("ip6_6in4_remote");
	
	if (is_interface_exist(IFNAME_SIT))
		doSystem("ip tunnel del %s", IFNAME_SIT);
	
	doSystem("ip tunnel %s %s mode sit remote %s local %s ttl %d", "add", IFNAME_SIT, sit_remote, wan_addr4, sit_ttl);
	
	if (ipv6_type == IPV6_6TO4) {
		size6 = 16;
		memset(&addr6, 0, sizeof(addr6));
		addr6.s6_addr16[0] = htons(0x2002);
		ipv6_to_ipv4_map(&addr6, size6, &addr4, 0);
		addr6.s6_addr16[7] = htons(0x0001);
		sit_relay = nvram_safe_get("ip6_6to4_relay");
	}
	else if (ipv6_type == IPV6_6RD) {
		memcpy(&net6, &addr6, sizeof(addr6));
		ipv6_to_net(&net6, size6);
		inet_ntop(AF_INET6, &net6, sit_6rd_prefix, INET6_ADDRSTRLEN);
		sprintf(sit_6rd_prefix, "%s/%d", sit_6rd_prefix, size6);
		
		strcpy(sit_6rd_relay_prefix, "0.0.0.0/0");
		size4 = nvram_get_int("wan0_6rd_size");
		if (size4 > 0 && size4 <= 32)
		{
			net4.s_addr = addr4.s_addr & htonl(0xffffffffUL << (32 - size4));
			sprintf(sit_6rd_relay_prefix, "%s/%d", inet_ntoa(net4), size4);
		}
		
		doSystem("ip tunnel 6rd dev %s 6rd-prefix %s 6rd-relay_prefix %s", IFNAME_SIT, sit_6rd_prefix, sit_6rd_relay_prefix);
		
		ipv6_to_ipv4_map(&addr6, size6, &addr4, size4);
		addr6.s6_addr16[7] = htons(0x0001);
		sit_relay = nvram_safe_get("wan0_6rd_relay");
	}

	// WAN IPv6 address
	inet_ntop(AF_INET6, &addr6, addr6s, INET6_ADDRSTRLEN);
	if (size6 > 0)
		sprintf(addr6s, "%s/%d", addr6s, size6);

	control_if_ipv6_radv(IFNAME_SIT, 0);
	doSystem("ip link set mtu %d dev %s up", sit_mtu, IFNAME_SIT);
	control_if_ipv6(IFNAME_SIT, 1);
	clear_if_addr6(IFNAME_SIT);
	doSystem("ip -6 addr add %s dev %s", addr6s, IFNAME_SIT);

	/* WAN IPv6 gateway (auto-generate for 6to4/6rd) */
	if (ipv6_type == IPV6_6TO4 || ipv6_type == IPV6_6RD) {
		sprintf(addr6s, "::%s", sit_relay);
		wan_gate6 = addr6s;
		/* add direct default gateway for workaround "No route to host" on new kernel */
		doSystem("ip -6 route add default dev %s metric %d", IFNAME_SIT, 2048);
	}
	else {
		wan_gate6 = nvram_safe_get("wan0_gate6");
	}
	if (*wan_gate6)
		doSystem("ip -6 route add default via %s dev %s metric %d", wan_gate6, IFNAME_SIT, 1);

	/* LAN IPv6 address (auto-generate for 6to4/6rd) */
	if (ipv6_type == IPV6_6TO4 || ipv6_type == IPV6_6RD) {
		memset(&addr6, 0, sizeof(addr6));
		if (ipv6_type == IPV6_6TO4) {
			size6 = 16;
			addr6.s6_addr16[0] = htons(0x2002);
			ipv6_to_ipv4_map(&addr6, size6, &addr4, 0);
			addr6.s6_addr16[3] = htons(0x0001);
			addr6.s6_addr16[7] = htons(0x0001);
		}
		else {
			ipv6_from_string(wan_addr6, &addr6);
			ipv6_to_ipv4_map(&addr6, 32, &addr4, size4);
			addr6.s6_addr16[7] = htons(0x0001);
		}
		
		inet_ntop(AF_INET6, &addr6, addr6s, INET6_ADDRSTRLEN);
		lan_size6 = nvram_get_int("ip6_lan_size");
		if (lan_size6 < 48 || lan_size6 > 80)
			lan_size6 = 64;
		sprintf(addr6s, "%s/%d", addr6s, lan_size6);
		
		clear_if_addr6(IFNAME_BR);
		doSystem("ip -6 addr add %s dev %s", addr6s, IFNAME_BR);
		
		update_lan_addr6_radv(addr6s);
	}
}

void stop_sit_tunnel(void)
{
	if (is_interface_exist(IFNAME_SIT))
		doSystem("ip tunnel del %s", IFNAME_SIT);
}

void wan6_up(char *wan_ifname)
{
	int ipv6_type, start_radvd_now;
	char *wan_addr6, *wan_gate6, *wan_addr4;

	ipv6_type = get_ipv6_type();
	if (ipv6_type == IPV6_DISABLED)
		return;

	stop_dhcp6c();

	build_dns6_var();

	control_if_ipv6_dad(IFNAME_BR, 1);

	start_radvd_now = 1;

	if (ipv6_type == IPV6_6IN4 || ipv6_type == IPV6_6TO4 || ipv6_type == IPV6_6RD)
	{
		wan_addr4 = nvram_safe_get("wan0_ipaddr");
		wan_addr6 = nvram_safe_get("wan0_addr6");
		start_sit_tunnel(ipv6_type, wan_addr4, wan_addr6);
	}
	else
	{
		control_if_ipv6_dad(wan_ifname, 1);
		
		if (ipv6_type == IPV6_NATIVE_STATIC) {
			wan_addr6 = nvram_safe_get("wan0_addr6");
			wan_gate6 = nvram_safe_get("wan0_gate6");
			control_if_ipv6_radv(wan_ifname, 0);
			clear_if_addr6(wan_ifname);
			if (*wan_addr6)
				doSystem("ip -6 addr add %s dev %s", wan_addr6, wan_ifname);
			if (*wan_gate6) {
				doSystem("ip -6 route add %s dev %s", wan_gate6, wan_ifname);
				doSystem("ip -6 route add default via %s metric %d", wan_gate6, 1);
			}
		}
		else {
			doSystem("ip -6 route add default dev %s metric %d", wan_ifname, 2048);
			control_if_ipv6_autoconf(wan_ifname, nvram_invmatch("ip6_wan_dhcp", "1"));
			control_if_ipv6_radv(wan_ifname, 1);
			/* wait for interface ready */
			sleep(2);
			start_dhcp6c(wan_ifname);
			if (nvram_match("ip6_lan_auto", "1"))
				start_radvd_now = 0;
		}
	}

	if (start_radvd_now)
		reload_radvd();
}

void wan6_down(char *wan_ifname)
{
	int ipv6_type;
	char *wan6_ifname;

	ipv6_type = get_ipv6_type();
	if (ipv6_type == IPV6_DISABLED)
		return;

	stop_radvd();
	stop_dhcp6c();
	control_if_ipv6_radv(wan_ifname, 0);
	control_if_ipv6_autoconf(wan_ifname, 0);

	if (ipv6_type == IPV6_6IN4 || ipv6_type == IPV6_6TO4 || ipv6_type == IPV6_6RD)
	{
		wan6_ifname = IFNAME_SIT;
		if (is_interface_exist(IFNAME_SIT))
			doSystem("ip link set dev %s down", IFNAME_SIT);
	}
	else
	{
		wan6_ifname = wan_ifname;
	}

	// clear WAN routes6
	clear_if_route6(wan6_ifname);

	// clear WAN addr6
	clear_if_addr6(wan6_ifname);

	// delete SIT tunnel
	stop_sit_tunnel();

	// clear DNS6 for resolv.conf
	nvram_set("wan0_dns6", "");
}

int dhcp6c_main(int argc, char **argv)
{
	int ipv6_type, dns6_auto, lan6_auto;
	char *dns6, *lan_addr6_new;
	char addr6s[INET6_ADDRSTRLEN];

//	char *wan_ifname = safe_getenv("interface");

	ipv6_type = get_ipv6_type();
	if (ipv6_type != IPV6_NATIVE_DHCP6)
		return 0;

	lan6_auto = nvram_get_int("ip6_lan_auto");
	if (lan6_auto) {
		lan_addr6_new = get_ifaddr6(IFNAME_BR, 0, addr6s);
		update_lan_addr6_radv(lan_addr6_new);
	}

	dns6_auto = nvram_get_int("ip6_dns_auto");
	if (dns6_auto) {
		dns6 = getenv("new_domain_name_servers");
		if (dns6 && nvram_invmatch("wan0_dns6", trim_r(dns6))) {
			nvram_set("wan0_dns6", trim_r(dns6));
			update_resolvconf(0, 0);
		}
	}

	reload_radvd();

	return 0;
}

int start_dhcp6c(char *wan_ifname)
{
	FILE *fp;
	int wan6_dhcp, dns6_auto, lan6_auto, ia_id, sla_id, sla_len;
	char *conf_file = "/etc/dhcp6c.conf";

	wan6_dhcp = nvram_get_int("ip6_wan_dhcp");
	dns6_auto = nvram_get_int("ip6_dns_auto");
	lan6_auto = nvram_get_int("ip6_lan_auto");
	if (!wan6_dhcp && !dns6_auto && !lan6_auto)
		return 1;

	ia_id = 0;
	sla_id = 1;
	sla_len = 0;
	sla_len = 64 - nvram_get_int("ip6_lan_size");
	if (sla_len <= 0) sla_len = 0;
	if (sla_len > 16) sla_len = 16;

	fp = fopen(conf_file, "w");
	if (!fp) {
		perror(conf_file);
		return -1;
	}

	fprintf(fp, "interface %s {\n", wan_ifname);
	if (wan6_dhcp)
		fprintf(fp, " send ia-na %d;\n", ia_id);
	if (lan6_auto)
		fprintf(fp, " send ia-pd %d;\n", ia_id);
	if (wan6_dhcp || lan6_auto)
		fprintf(fp, " send rapid-commit;\n");
	else
		fprintf(fp, " information-only;\n");
	if (dns6_auto)
		fprintf(fp, " request domain-name-servers;\n");
	fprintf(fp,
		" script \"%s\";\n"
		"};\n",
		SCRIPT_DHCP6C_WAN
	);
	if (wan6_dhcp)
		fprintf(fp, "id-assoc na %d { };\n", ia_id);
	if (lan6_auto)
		fprintf(fp,
			"id-assoc pd %d {\n"
			" prefix-interface %s {\n"
			"  sla-id %d;\n"
			"  sla-len %d;\n"
			" };\n"
			"};\n",
			ia_id,
			IFNAME_BR,
			sla_id,
			sla_len
		);

	fclose(fp);

	logmessage("DHCPv6 WAN Client", "starting on wan (%s) ...", wan_ifname);

	return eval("/sbin/dhcp6c", "-D", "LL", wan_ifname);
}

void stop_dhcp6c(void)
{
	char *svcs[] = { "dhcp6c", NULL };
	kill_services(svcs, 3, 1);
}

#endif

