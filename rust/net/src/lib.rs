#![no_std]

use core::ffi::{c_char, CStr};
use core::mem::MaybeUninit;
use core::panic::PanicInfo;
use smoltcp::iface::{Config, Interface, SocketSet, SocketStorage};
use smoltcp::phy::{Device, DeviceCapabilities, Medium, RxToken, TxToken};
use smoltcp::socket::{dhcpv4, icmp, tcp, udp};
use smoltcp::time::Instant;
use smoltcp::wire::{
    EthernetAddress, HardwareAddress, IpAddress, IpCidr, IpEndpoint, Ipv4Address, Ipv4Cidr,
};

const MAX_FRAME_SIZE: usize = 2048;
const SOCKET_COUNT: usize = 4;
const DHCP_BUFFER_SIZE: usize = 1500;
const ICMP_BUFFER_SIZE: usize = 512;
const TCP_BUFFER_SIZE: usize = 2048;
const DNS_RX_BUFFER_SIZE: usize = 1024;
const DNS_TX_BUFFER_SIZE: usize = 512;
const PING_COUNT: usize = 4;
const PING_TIMEOUT_MS: u64 = 1000;
const TCP_LOCAL_PORT: u16 = 49152;
const TCP_STREAM_TIMEOUT_MS: u64 = 15000;

unsafe extern "C" {
    fn os_net_e1000_receive(buffer: *mut u8, capacity: usize) -> i32;
    fn os_net_e1000_send(buffer: *const u8, length: usize) -> bool;
    fn os_net_clock_ms() -> u64;
    fn os_net_yield();
}

struct E1000Device {
    rx_buffer: [u8; MAX_FRAME_SIZE],
}

struct E1000RxToken<'a> {
    buffer: &'a mut [u8],
}

struct E1000TxToken;

impl Device for E1000Device {
    type RxToken<'a>
        = E1000RxToken<'a>
    where
        Self: 'a;
    type TxToken<'a>
        = E1000TxToken
    where
        Self: 'a;

    fn receive(&mut self, _timestamp: Instant) -> Option<(Self::RxToken<'_>, Self::TxToken<'_>)> {
        let length =
            unsafe { os_net_e1000_receive(self.rx_buffer.as_mut_ptr(), self.rx_buffer.len()) };
        if length <= 0 || length as usize > self.rx_buffer.len() {
            return None;
        }

        let length = length as usize;
        Some((
            E1000RxToken {
                buffer: &mut self.rx_buffer[..length],
            },
            E1000TxToken,
        ))
    }

    fn transmit(&mut self, _timestamp: Instant) -> Option<Self::TxToken<'_>> {
        Some(E1000TxToken)
    }

    fn capabilities(&self) -> DeviceCapabilities {
        let mut capabilities = DeviceCapabilities::default();
        capabilities.medium = Medium::Ethernet;
        capabilities.max_transmission_unit = 1500;
        capabilities
    }
}

impl RxToken for E1000RxToken<'_> {
    fn consume<R, F>(self, receive: F) -> R
    where
        F: FnOnce(&[u8]) -> R,
    {
        receive(self.buffer)
    }
}

impl TxToken for E1000TxToken {
    fn consume<R, F>(self, length: usize, transmit: F) -> R
    where
        F: FnOnce(&mut [u8]) -> R,
    {
        let frame_length = length.min(MAX_FRAME_SIZE);
        let mut buffer = [0; MAX_FRAME_SIZE];
        let result = transmit(&mut buffer[..frame_length]);
        unsafe {
            os_net_e1000_send(buffer.as_ptr(), frame_length);
        }
        result
    }
}

#[repr(C)]
pub struct NetInfo {
    configured: u8,
    prefix_len: u8,
    mac: [u8; 6],
    ipv4: u32,
    gateway: u32,
    dns: u32,
}

#[repr(C)]
pub struct PingStats {
    sent: u32,
    received: u32,
    received_mask: u32,
    rtt_ms: [u32; PING_COUNT],
}

struct NetState {
    device: E1000Device,
    interface: Interface,
    sockets: SocketSet<'static>,
    dhcp_handle: smoltcp::iface::SocketHandle,
    dns_handle: smoltcp::iface::SocketHandle,
    icmp_handle: smoltcp::iface::SocketHandle,
    tcp_handle: smoltcp::iface::SocketHandle,
    mac: [u8; 6],
    configured: bool,
    address: Option<Ipv4Cidr>,
    gateway: Option<Ipv4Address>,
    dns_server: Option<Ipv4Address>,
    next_local_port: u16,
}

static mut SOCKET_STORAGE: [SocketStorage<'static>; SOCKET_COUNT] =
    [const { SocketStorage::EMPTY }; SOCKET_COUNT];
static mut DHCP_RX_BUFFER: [u8; DHCP_BUFFER_SIZE] = [0; DHCP_BUFFER_SIZE];
static mut DNS_RX_METADATA: [udp::PacketMetadata; 4] = [udp::PacketMetadata::EMPTY; 4];
static mut DNS_TX_METADATA: [udp::PacketMetadata; 4] = [udp::PacketMetadata::EMPTY; 4];
static mut DNS_RX_BUFFER: [u8; DNS_RX_BUFFER_SIZE] = [0; DNS_RX_BUFFER_SIZE];
static mut DNS_TX_BUFFER: [u8; DNS_TX_BUFFER_SIZE] = [0; DNS_TX_BUFFER_SIZE];
static mut ICMP_RX_METADATA: [icmp::PacketMetadata; 4] = [icmp::PacketMetadata::EMPTY; 4];
static mut ICMP_TX_METADATA: [icmp::PacketMetadata; 4] = [icmp::PacketMetadata::EMPTY; 4];
static mut ICMP_RX_BUFFER: [u8; ICMP_BUFFER_SIZE] = [0; ICMP_BUFFER_SIZE];
static mut ICMP_TX_BUFFER: [u8; ICMP_BUFFER_SIZE] = [0; ICMP_BUFFER_SIZE];
static mut TCP_RX_BUFFER: [u8; TCP_BUFFER_SIZE] = [0; TCP_BUFFER_SIZE];
static mut TCP_TX_BUFFER: [u8; TCP_BUFFER_SIZE] = [0; TCP_BUFFER_SIZE];
static mut STATE: MaybeUninit<NetState> = MaybeUninit::uninit();
static mut STATE_READY: bool = false;

unsafe fn state_mut() -> Option<&'static mut NetState> {
    if core::ptr::read_volatile(core::ptr::addr_of!(STATE_READY)) {
        Some((*core::ptr::addr_of_mut!(STATE)).assume_init_mut())
    } else {
        None
    }
}

fn now() -> Instant {
    let milliseconds = unsafe { os_net_clock_ms() };
    Instant::from_millis(milliseconds.min(i64::MAX as u64) as i64)
}

fn poll_once(state: &mut NetState) {
    let timestamp = now();
    state
        .interface
        .poll(timestamp, &mut state.device, &mut state.sockets);

    enum DhcpUpdate {
        None,
        Configured(Ipv4Cidr, Option<Ipv4Address>, Option<Ipv4Address>),
        Deconfigured,
    }

    let update = {
        let dhcp = state.sockets.get_mut::<dhcpv4::Socket>(state.dhcp_handle);
        match dhcp.poll() {
            Some(dhcpv4::Event::Configured(config)) => DhcpUpdate::Configured(
                config.address,
                config.router,
                config.dns_servers.first().copied(),
            ),
            Some(dhcpv4::Event::Deconfigured) => DhcpUpdate::Deconfigured,
            None => DhcpUpdate::None,
        }
    };

    match update {
        DhcpUpdate::None => {}
        DhcpUpdate::Configured(address, gateway, dns_server) => {
            state.interface.update_ip_addrs(|addresses| {
                addresses.clear();
                let _ = addresses.push(IpCidr::Ipv4(address));
            });
            state.interface.routes_mut().remove_default_ipv4_route();
            if let Some(gateway) = gateway {
                let _ = state.interface.routes_mut().add_default_ipv4_route(gateway);
            }
            state.address = Some(address);
            state.gateway = gateway;
            state.dns_server = dns_server;
            state.configured = true;
        }
        DhcpUpdate::Deconfigured => {
            state
                .interface
                .update_ip_addrs(|addresses| addresses.clear());
            state.interface.routes_mut().remove_default_ipv4_route();
            state.address = None;
            state.gateway = None;
            state.dns_server = None;
            state.configured = false;
        }
    }
}

fn wait_configured(state: &mut NetState, timeout_ms: u64) -> bool {
    let started = unsafe { os_net_clock_ms() };
    loop {
        poll_once(state);
        if state.configured {
            return true;
        }
        if unsafe { os_net_clock_ms() }.wrapping_sub(started) >= timeout_ms {
            return false;
        }
        unsafe { os_net_yield() };
    }
}

fn parse_ipv4(text: &str) -> Option<Ipv4Address> {
    let mut octets = [0u8; 4];
    let mut part = 0usize;
    let mut value = 0u16;
    let mut digits = 0usize;
    for byte in text.bytes() {
        if byte == b'.' {
            if digits == 0 || part == 3 || value > 255 {
                return None;
            }
            octets[part] = value as u8;
            part += 1;
            value = 0;
            digits = 0;
        } else if byte.is_ascii_digit() {
            value = value * 10 + (byte - b'0') as u16;
            if value > 255 {
                return None;
            }
            digits += 1;
        } else {
            return None;
        }
    }
    if part != 3 || digits == 0 || value > 255 {
        return None;
    }
    octets[part] = value as u8;
    Some(Ipv4Address::new(octets[0], octets[1], octets[2], octets[3]))
}

fn resolve_ipv4(state: &mut NetState, host: &str, timeout_ms: u64) -> Option<Ipv4Address> {
    if let Some(address) = parse_ipv4(host) {
        return Some(address);
    }
    let dns_server = state.dns_server?;
    let mut packet = [0u8; 512];
    packet[0] = 0x4f;
    packet[1] = 0x53;
    packet[2] = 0x01;
    packet[5] = 0x01;
    let mut offset = 12usize;
    for label in host.split('.') {
        if label.is_empty() || label.len() > 63 || offset + label.len() + 6 > packet.len() {
            return None;
        }
        packet[offset] = label.len() as u8;
        offset += 1;
        packet[offset..offset + label.len()].copy_from_slice(label.as_bytes());
        offset += label.len();
    }
    packet[offset] = 0;
    offset += 1;
    packet[offset..offset + 4].copy_from_slice(&[0, 1, 0, 1]);
    offset += 4;
    {
        let socket = state.sockets.get_mut::<udp::Socket>(state.dns_handle);
        socket
            .send_slice(
                &packet[..offset],
                IpEndpoint::new(IpAddress::Ipv4(dns_server), 53),
            )
            .ok()?;
    }
    let started = unsafe { os_net_clock_ms() };
    loop {
        poll_once(state);
        let mut response = [0u8; DNS_RX_BUFFER_SIZE];
        if let Ok((length, endpoint)) = state
            .sockets
            .get_mut::<udp::Socket>(state.dns_handle)
            .recv_slice(&mut response)
        {
            if endpoint.endpoint.addr == IpAddress::Ipv4(dns_server)
                && endpoint.endpoint.port == 53
                && length >= 12
                && response[0..2] == [0x4f, 0x53]
                && response[2] & 0x80 != 0
                && response[3] & 0x0f == 0
            {
                let questions = u16::from_be_bytes([response[4], response[5]]);
                let answers = u16::from_be_bytes([response[6], response[7]]);
                let mut cursor = 12usize;
                for _ in 0..questions {
                    if !skip_dns_name(&response[..length], &mut cursor) || cursor + 4 > length {
                        return None;
                    }
                    cursor += 4;
                }
                for _ in 0..answers {
                    if !skip_dns_name(&response[..length], &mut cursor) || cursor + 10 > length {
                        return None;
                    }
                    let record_type = u16::from_be_bytes([response[cursor], response[cursor + 1]]);
                    let record_class =
                        u16::from_be_bytes([response[cursor + 2], response[cursor + 3]]);
                    let record_length =
                        u16::from_be_bytes([response[cursor + 8], response[cursor + 9]]) as usize;
                    cursor += 10;
                    if cursor + record_length > length {
                        return None;
                    }
                    if record_type == 1 && record_class == 1 && record_length == 4 {
                        return Some(Ipv4Address::new(
                            response[cursor],
                            response[cursor + 1],
                            response[cursor + 2],
                            response[cursor + 3],
                        ));
                    }
                    cursor += record_length;
                }
                return None;
            }
        }
        if unsafe { os_net_clock_ms() }.wrapping_sub(started) >= timeout_ms {
            return None;
        }
        unsafe { os_net_yield() };
    }
}

fn skip_dns_name(packet: &[u8], offset: &mut usize) -> bool {
    let mut cursor = *offset;
    loop {
        if cursor >= packet.len() {
            return false;
        }
        let length = packet[cursor];
        cursor += 1;
        if length == 0 {
            *offset = cursor;
            return true;
        }
        if length & 0xc0 == 0xc0 {
            if cursor >= packet.len() {
                return false;
            }
            *offset = cursor + 1;
            return true;
        }
        if length & 0xc0 != 0 || cursor + length as usize > packet.len() {
            return false;
        }
        cursor += length as usize;
    }
}

fn icmp_checksum(packet: &[u8]) -> u16 {
    let mut sum = 0u32;
    let mut chunks = packet.chunks_exact(2);
    for pair in &mut chunks {
        sum += u16::from_be_bytes([pair[0], pair[1]]) as u32;
    }
    if let Some(byte) = chunks.remainder().first() {
        sum += (*byte as u32) << 8;
    }
    while sum >> 16 != 0 {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    !(sum as u16)
}

#[no_mangle]
pub extern "C" fn osfos_net_init(mac: *const u8, seed: u64) -> i32 {
    if mac.is_null() {
        return -1;
    }
    if unsafe { state_mut() }.is_some() {
        return 0;
    }

    let mut mac_address = [0u8; 6];
    unsafe { core::ptr::copy_nonoverlapping(mac, mac_address.as_mut_ptr(), 6) };
    let device = E1000Device {
        rx_buffer: [0; MAX_FRAME_SIZE],
    };
    let mut config = Config::new(HardwareAddress::Ethernet(EthernetAddress(mac_address)));
    config.random_seed = seed;
    let mut device = device;
    let interface = Interface::new(config, &mut device, now());

    let storage = unsafe { &mut *core::ptr::addr_of_mut!(SOCKET_STORAGE) };
    let mut sockets = SocketSet::new(&mut storage[..]);

    let mut dhcp_socket = dhcpv4::Socket::new();
    let dhcp_rx = unsafe { &mut *core::ptr::addr_of_mut!(DHCP_RX_BUFFER) };
    dhcp_socket.set_receive_packet_buffer(dhcp_rx);
    let dhcp_handle = sockets.add(dhcp_socket);

    let dns_rx_metadata = unsafe { &mut *core::ptr::addr_of_mut!(DNS_RX_METADATA) };
    let dns_tx_metadata = unsafe { &mut *core::ptr::addr_of_mut!(DNS_TX_METADATA) };
    let dns_rx_buffer = unsafe { &mut *core::ptr::addr_of_mut!(DNS_RX_BUFFER) };
    let dns_tx_buffer = unsafe { &mut *core::ptr::addr_of_mut!(DNS_TX_BUFFER) };
    let dns_rx = udp::PacketBuffer::new(&mut dns_rx_metadata[..], &mut dns_rx_buffer[..]);
    let dns_tx = udp::PacketBuffer::new(&mut dns_tx_metadata[..], &mut dns_tx_buffer[..]);
    let mut dns_socket = udp::Socket::new(dns_rx, dns_tx);
    if dns_socket.bind(53000).is_err() {
        return -2;
    }
    let dns_handle = sockets.add(dns_socket);

    let icmp_rx_metadata = unsafe { &mut *core::ptr::addr_of_mut!(ICMP_RX_METADATA) };
    let icmp_tx_metadata = unsafe { &mut *core::ptr::addr_of_mut!(ICMP_TX_METADATA) };
    let icmp_rx_buffer = unsafe { &mut *core::ptr::addr_of_mut!(ICMP_RX_BUFFER) };
    let icmp_tx_buffer = unsafe { &mut *core::ptr::addr_of_mut!(ICMP_TX_BUFFER) };
    let icmp_rx = icmp::PacketBuffer::new(&mut icmp_rx_metadata[..], &mut icmp_rx_buffer[..]);
    let icmp_tx = icmp::PacketBuffer::new(&mut icmp_tx_metadata[..], &mut icmp_tx_buffer[..]);
    let mut icmp_socket = icmp::Socket::new(icmp_rx, icmp_tx);
    if icmp_socket.bind(icmp::Endpoint::Ident(0x4F53)).is_err() {
        return -2;
    }
    let icmp_handle = sockets.add(icmp_socket);

    let tcp_rx_buffer = unsafe { &mut *core::ptr::addr_of_mut!(TCP_RX_BUFFER) };
    let tcp_tx_buffer = unsafe { &mut *core::ptr::addr_of_mut!(TCP_TX_BUFFER) };
    let tcp_socket = tcp::Socket::new(
        tcp::SocketBuffer::new(&mut tcp_rx_buffer[..]),
        tcp::SocketBuffer::new(&mut tcp_tx_buffer[..]),
    );
    let tcp_handle = sockets.add(tcp_socket);

    let state = NetState {
        device,
        interface,
        sockets,
        dhcp_handle,
        dns_handle,
        icmp_handle,
        tcp_handle,
        mac: mac_address,
        configured: false,
        address: None,
        gateway: None,
        dns_server: None,
        next_local_port: TCP_LOCAL_PORT,
    };
    unsafe {
        (*core::ptr::addr_of_mut!(STATE)).write(state);
        core::ptr::write_volatile(core::ptr::addr_of_mut!(STATE_READY), true);
    }
    0
}

#[no_mangle]
pub extern "C" fn osfos_net_poll() -> i32 {
    let Some(state) = (unsafe { state_mut() }) else {
        return -1;
    };
    poll_once(state);
    0
}

#[no_mangle]
pub extern "C" fn osfos_net_configure(timeout_ms: u32) -> i32 {
    let Some(state) = (unsafe { state_mut() }) else {
        return -1;
    };
    if wait_configured(state, timeout_ms as u64) {
        0
    } else {
        -2
    }
}

#[no_mangle]
pub extern "C" fn osfos_net_info(output: *mut NetInfo) -> i32 {
    if output.is_null() {
        return -1;
    }
    let Some(state) = (unsafe { state_mut() }) else {
        return -2;
    };
    let mut info = NetInfo {
        configured: u8::from(state.configured),
        prefix_len: state
            .address
            .map(|address| address.prefix_len())
            .unwrap_or(0),
        mac: state.mac,
        ipv4: 0,
        gateway: 0,
        dns: 0,
    };
    if let Some(address) = state.address {
        let octets = address.address().octets();
        info.ipv4 = u32::from_be_bytes(octets);
    }
    if let Some(gateway) = state.gateway {
        info.gateway = u32::from_be_bytes(gateway.octets());
    }
    if let Some(dns) = state.dns_server {
        info.dns = u32::from_be_bytes(dns.octets());
    }
    unsafe { output.write(info) };
    0
}

#[no_mangle]
pub extern "C" fn osfos_net_ping(host: *const c_char, output: *mut PingStats) -> i32 {
    if host.is_null() || output.is_null() {
        return -1;
    }
    let Ok(host) = (unsafe { CStr::from_ptr(host) }).to_str() else {
        return -1;
    };
    let Some(state) = (unsafe { state_mut() }) else {
        return -2;
    };
    if !wait_configured(state, 8000) {
        return -3;
    }
    let Some(destination) = resolve_ipv4(state, host, 5000) else {
        return -4;
    };

    let mut stats = PingStats {
        sent: 0,
        received: 0,
        received_mask: 0,
        rtt_ms: [0; PING_COUNT],
    };
    for sequence in 0..PING_COUNT {
        let mut request = [0u8; 32];
        request[0] = 8;
        request[4..6].copy_from_slice(&0x4F53u16.to_be_bytes());
        request[6..8].copy_from_slice(&((sequence + 1) as u16).to_be_bytes());
        request[8..12].copy_from_slice(b"OSFS");
        request[12..16].copy_from_slice(&(sequence as u32).to_be_bytes());
        for (index, byte) in request[16..].iter_mut().enumerate() {
            *byte = (index as u8).wrapping_add(sequence as u8);
        }
        let attempts = if sequence == 0 { 2 } else { 1 };
        let mut queued = false;
        for attempt in 0..attempts {
            request[16] = attempt as u8;
            request[2..4].fill(0);
            let checksum = icmp_checksum(&request);
            request[2..4].copy_from_slice(&checksum.to_be_bytes());
            let send_result = state
                .sockets
                .get_mut::<icmp::Socket>(state.icmp_handle)
                .send_slice(&request, IpAddress::Ipv4(destination));
            if send_result.is_err() {
                continue;
            }
            if !queued {
                stats.sent += 1;
                queued = true;
            }
            let started = unsafe { os_net_clock_ms() };
            loop {
                poll_once(state);
                let mut received = [0u8; 64];
                let result = state
                    .sockets
                    .get_mut::<icmp::Socket>(state.icmp_handle)
                    .recv_slice(&mut received);
                if let Ok((length, source)) = result {
                    if source == IpAddress::Ipv4(destination)
                        && length >= request.len()
                        && received[0] == 0
                        && received[1] == 0
                        && received[4..8] == request[4..8]
                        && received[8..request.len()] == request[8..]
                    {
                        stats.received += 1;
                        stats.received_mask |= 1 << sequence;
                        stats.rtt_ms[sequence] = unsafe { os_net_clock_ms() }
                            .wrapping_sub(started)
                            .min(u32::MAX as u64)
                            as u32;
                        break;
                    }
                }
                if unsafe { os_net_clock_ms() }.wrapping_sub(started) >= PING_TIMEOUT_MS {
                    break;
                }
                unsafe { os_net_yield() };
            }
            if stats.received_mask & (1 << sequence) != 0 {
                break;
            }
        }
        if sequence + 1 < PING_COUNT {
            unsafe { os_net_yield() };
        }
    }
    unsafe { output.write(stats) };
    0
}

#[no_mangle]
pub extern "C" fn osfos_net_tcp_echo(
    host: *const c_char,
    port: u16,
    message: *const u8,
    message_length: usize,
    output: *mut u8,
    output_capacity: usize,
    timeout_ms: u32,
) -> i32 {
    if host.is_null() || message.is_null() || output.is_null() || port == 0 {
        return -1;
    }
    let Ok(host) = (unsafe { CStr::from_ptr(host) }).to_str() else {
        return -1;
    };
    let Some(state) = (unsafe { state_mut() }) else {
        return -2;
    };
    if !wait_configured(state, 8000) {
        return -3;
    }
    let Some(destination) = resolve_ipv4(state, host, 5000) else {
        return -4;
    };

    let connect_result = {
        let context = state.interface.context();
        let socket = state.sockets.get_mut::<tcp::Socket>(state.tcp_handle);
        if socket.is_open() {
            socket.abort();
        }
        socket.set_timeout(Some(smoltcp::time::Duration::from_millis(
            timeout_ms as u64,
        )));
        socket.connect(
            context,
            IpEndpoint::new(IpAddress::Ipv4(destination), port),
            TCP_LOCAL_PORT,
        )
    };
    if connect_result.is_err() {
        return -5;
    }

    let started = unsafe { os_net_clock_ms() };
    loop {
        poll_once(state);
        let socket = state.sockets.get::<tcp::Socket>(state.tcp_handle);
        if socket.state() == tcp::State::Established {
            break;
        }
        if !socket.is_active()
            || unsafe { os_net_clock_ms() }.wrapping_sub(started) >= timeout_ms as u64
        {
            state
                .sockets
                .get_mut::<tcp::Socket>(state.tcp_handle)
                .abort();
            return -6;
        }
        unsafe { os_net_yield() };
    }

    let message_length = message_length.min(TCP_BUFFER_SIZE);
    let sent = state
        .sockets
        .get_mut::<tcp::Socket>(state.tcp_handle)
        .send_slice(unsafe { core::slice::from_raw_parts(message, message_length) });
    if sent.is_err() || sent.unwrap_or(0) != message_length {
        state
            .sockets
            .get_mut::<tcp::Socket>(state.tcp_handle)
            .abort();
        return -7;
    }

    let started = unsafe { os_net_clock_ms() };
    let mut received = 0usize;
    loop {
        poll_once(state);
        let socket = state.sockets.get_mut::<tcp::Socket>(state.tcp_handle);
        if socket.can_recv() && received < output_capacity {
            let output_slice = unsafe {
                core::slice::from_raw_parts_mut(output.add(received), output_capacity - received)
            };
            match socket.recv_slice(output_slice) {
                Ok(length) => received += length,
                Err(_) => break,
            }
        }
        if received > 0 && !socket.may_recv() {
            break;
        }
        if !socket.is_active()
            || unsafe { os_net_clock_ms() }.wrapping_sub(started) >= timeout_ms as u64
        {
            break;
        }
        unsafe { os_net_yield() };
    }
    state
        .sockets
        .get_mut::<tcp::Socket>(state.tcp_handle)
        .close();
    poll_once(state);
    received.min(i32::MAX as usize) as i32
}

#[no_mangle]
pub extern "C" fn osfos_net_tcp_open(host: *const c_char, port: u16, timeout_ms: u32) -> i32 {
    if host.is_null() || port == 0 {
        return -1;
    }
    let Ok(host) = (unsafe { CStr::from_ptr(host) }).to_str() else {
        return -1;
    };
    let Some(state) = (unsafe { state_mut() }) else {
        return -2;
    };
    if !wait_configured(state, 8000) {
        return -3;
    }
    let Some(destination) = resolve_ipv4(state, host, 5000) else {
        return -4;
    };

    let local_port = state.next_local_port;
    state.next_local_port = state.next_local_port.wrapping_add(1).max(TCP_LOCAL_PORT);
    let connect_result = {
        let context = state.interface.context();
        let socket = state.sockets.get_mut::<tcp::Socket>(state.tcp_handle);
        if socket.is_open() {
            socket.abort();
        }
        socket.set_timeout(Some(smoltcp::time::Duration::from_millis(
            timeout_ms as u64,
        )));
        socket.connect(
            context,
            IpEndpoint::new(IpAddress::Ipv4(destination), port),
            local_port,
        )
    };
    if connect_result.is_err() {
        return -5;
    }

    let started = unsafe { os_net_clock_ms() };
    loop {
        poll_once(state);
        let socket = state.sockets.get::<tcp::Socket>(state.tcp_handle);
        if socket.state() == tcp::State::Established {
            return 0;
        }
        if !socket.is_active()
            || unsafe { os_net_clock_ms() }.wrapping_sub(started) >= timeout_ms as u64
        {
            state
                .sockets
                .get_mut::<tcp::Socket>(state.tcp_handle)
                .abort();
            return -6;
        }
        unsafe { os_net_yield() };
    }
}

#[no_mangle]
pub extern "C" fn osfos_net_tcp_write(data: *const u8, length: usize) -> i32 {
    if data.is_null() || length > TCP_BUFFER_SIZE {
        return -1;
    }
    let Some(state) = (unsafe { state_mut() }) else {
        return -2;
    };
    let bytes = unsafe { core::slice::from_raw_parts(data, length) };
    let started = unsafe { os_net_clock_ms() };
    let mut written = 0usize;
    while written < length {
        poll_once(state);
        let socket = state.sockets.get_mut::<tcp::Socket>(state.tcp_handle);
        if !socket.can_send() {
            if !socket.is_active()
                || unsafe { os_net_clock_ms() }.wrapping_sub(started) >= TCP_STREAM_TIMEOUT_MS
            {
                return -3;
            }
            unsafe { os_net_yield() };
            continue;
        }
        match socket.send_slice(&bytes[written..]) {
            Ok(0) => unsafe { os_net_yield() },
            Ok(count) => written += count,
            Err(_) => return -4,
        }
    }
    written.min(i32::MAX as usize) as i32
}

#[no_mangle]
pub extern "C" fn osfos_net_tcp_read(output: *mut u8, capacity: usize, timeout_ms: u32) -> i32 {
    if output.is_null() || capacity == 0 {
        return -1;
    }
    let Some(state) = (unsafe { state_mut() }) else {
        return -2;
    };
    let started = unsafe { os_net_clock_ms() };
    loop {
        poll_once(state);
        let socket = state.sockets.get_mut::<tcp::Socket>(state.tcp_handle);
        if socket.can_recv() {
            let count = capacity.min(TCP_BUFFER_SIZE);
            let output = unsafe { core::slice::from_raw_parts_mut(output, count) };
            return socket
                .recv_slice(output)
                .map(|count| count as i32)
                .unwrap_or(-3);
        }
        if !socket.may_recv() || !socket.is_active() {
            return 0;
        }
        if unsafe { os_net_clock_ms() }.wrapping_sub(started) >= timeout_ms as u64 {
            return -4;
        }
        unsafe { os_net_yield() };
    }
}

#[no_mangle]
pub extern "C" fn osfos_net_tcp_close() -> i32 {
    let Some(state) = (unsafe { state_mut() }) else {
        return -1;
    };
    state
        .sockets
        .get_mut::<tcp::Socket>(state.tcp_handle)
        .close();
    let started = unsafe { os_net_clock_ms() };
    loop {
        poll_once(state);
        let socket = state.sockets.get::<tcp::Socket>(state.tcp_handle);
        if !socket.is_active() {
            return 0;
        }
        if unsafe { os_net_clock_ms() }.wrapping_sub(started) >= 3000 {
            state
                .sockets
                .get_mut::<tcp::Socket>(state.tcp_handle)
                .abort();
            return -2;
        }
        unsafe { os_net_yield() };
    }
}

#[panic_handler]
fn panic(_info: &PanicInfo<'_>) -> ! {
    loop {
        unsafe {
            core::arch::asm!("cli; hlt", options(nomem, nostack));
        }
    }
}
