import requests, socket
import subprocess
import asyncio
import re, os, time, glob
from collections import Counter
from typing import Tuple
from datetime import datetime


def mkdir(path: str):
    if not os.path.isdir(path):
        os.mkdir(path)


def rm_files(dirpath: str, pattern: str = "*"):
    for file in glob.glob(os.path.join(dirpath, pattern)):
        if os.path.isfile(file):
            os.remove(file)


def title(msg: str, timestamped: bool = False):
    if timestamped:
        d = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        tstamp = f"({d})"
    else:
        tstamp = ''
    _msg = f"*** {msg} ***"
    print("\n\n%-40s%s" % (_msg, tstamp))
    print('=' * (8+len(msg)))

def get_epoch_ms() -> int:
    """
    Get epoch time in ms
    """
    return int(time.time()*1_000)

def dn_prefix(dn_start: int, n_ports: int) -> Tuple[str, int]:
    """
    The common prefix of [dn_start, dn_start+n_ports] range,
    and the number of the changing suffix digits
    """
    dn_end = dn_start + n_ports
    str_start = str(dn_start)
    str_end = str(dn_end)
    for i, (c1, c2) in enumerate(zip(str_start, str_end)):
        if c1 != c2:
            break
    return (str_start[:i], len(str_end)-i)

def get_next_port(init_port: int):
    """
    A closure to return the next 1 increment
    """
    port = init_port-1
    def increment():
        nonlocal port
        port += 1
        return port
    return increment

def check_running_pid(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    else:
        return True


def translate_time_signature(time_sign: str) -> int:
    """
    Translate time signrature of the form e.g. 3h24m23s346ms
    to milliseconds. 's' is the default unit.
    """
    if re.match(r"\d+$", time_sign):
        return int(time_sign) * 1_000
    h,m,s,ms = re.match(r"(?:(\d+)h)?(?:(\d+)m(?!s))?(?:(\d+)s)?(?:(\d+)ms)?",
                        time_sign.lower()).groups()
    out_msec = int(h or 0) * 3600_000
    out_msec += int(m or 0) * 60_000
    out_msec += int(s or 0) * 1_000
    out_msec += int(ms or 0)

    return out_msec

def check_no_duplicate(input: list) -> bool:
    """
    Return True if input list contains no duplicates
    """
    if not input:
        return True
    c = Counter(input)
    item,occur = c.most_common(1)[0]
    if occur > 1:
        print(f"ERROR: Duplicate use of SIP socket: {item}")
        return False
    return True


def check_overlap(ranges: dict) -> bool:
    """
    Validate that no ranges overlap.
    ranges is a dictionary of {media_server: [(start,end, "range-name"),...], ...}
    """
    for k, v in ranges.items():
        n_ranges = len(v)
        match n_ranges:
            case 0:
                print(f"{k}: OK - No ports selected")
                continue
            case 1:
                start,end,name = v[0]
                n_ports = end-start+1
                print(f"{k}: OK - {n_ports} ports: {name} [{start},{end}]")
                continue

            case _:
                v.sort(key=lambda x: x[0])
                n_ports = 0; temp_str = ""
                for i in range(len(v)-1):
                    start1,end1, name1 = v[i]
                    start2,end2,name2 = v[i+1]
                    if start2 <= end1:
                        print(f"{k}: ERROR - Port range overlap: {name1} [{start1},{end1}] and {name2} [{start2},{end2}]")
                        return True
                    n_ports += end1-start1+1
                    temp_str += f"{name1} [{start1},{end1}] "
                n_ports += end2-start2+1
                print(f"{k}: OK - {n_ports} ports: {temp_str}and {name2} [{start2},{end2}]")

    return False


def spawn_process(argv: list, background: bool = True) -> int:
    """
    Spawn a (sipp) process in the background or synchronously.
    Return:
        - pid, or 0 if it failed (background case)
        - return code (foreground case)
    """
    if background:
        p = subprocess.Popen(argv, stdout=subprocess.PIPE)
        time.sleep(.5)
        stdout, _ = p.communicate()
        r = re.findall(r"PID=\[(\d+)\]", stdout.decode("utf-8"))
        if not r:
            return 0
        pid = int(r[0])
        if check_running_pid(pid):
            return pid
        else:
            return 0
    else:
       process = subprocess.Popen(argv, stdout=subprocess.DEVNULL)
       return process.wait()


def get_primary_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect(("8.8.8.8", 80))  # Connects to Google's DNS (doesn't send data)
    ip_address = s.getsockname()[0]
    s.close()
    return ip_address


def check_server(servers: dict) -> dict:
    """
    Check the availability of media servers.
    Return a dictionary of only the responding servers
    """
    output = dict()
    for server in servers:
        for ms, endpoint in server.items():
            url = f"http://{endpoint}/status"
            str_ms = f"{ms}: {url}"
            try:
                response = requests.get(url, timeout=1)
                if response.status_code == 200:
                    print(f"{str_ms} OK")
                    output[ms] = endpoint
                else:
                    print(f"{str_ms} ERROR")

            except Exception as err:
                # print(err)
                print(f"{str_ms} ERROR")

    return output


def send_control_cmd(cmd: str, port: int, ip_addr: str = "127.0.0.1", opt: int = None):
    """
    Send a control command via UDP socket.
    Optional 'opt' can set values to interactive commands.
    """
    match (cmd):
        case "quit":
            bCmd = b'q'
        case "fQuit":
            bCmd = b'Q'
        case "pause":
            bCmd = b'p'
        case "incr_1":
            bCmd = b'+'
        case "decr_1":
            bCmd = b'-'
        case "incr_10":
            bCmd = b'*'
        case "decr_10":
            bCmd = b'/'
        case "setRate":
            bCmd = f"cset rate {opt}".encode("ASCII")
        case _:
            print(f"Unknown command '{cmd}")
            return
    upd_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    upd_socket.sendto(bCmd, (ip_addr, port))
    upd_socket.close()


async def stop_scenario(scen: dict) -> bool:
    """
    Gracefully terminate a scenario.
    If it still runs, after more than call-duration time, try to kill it.
    Return False, if everything fail
    """
    TIMEOUT = 5
    for k, v in scen.items():
        pid = v.get("pid")
        if not pid:
            return True
        print(f"Wait up to {v["duration"]}s to terminate {k}")
        send_control_cmd("quit", v["cPort"], v["ip"])
        wait_time = v["duration"] + TIMEOUT
        for i in range(0, wait_time, TIMEOUT):
            await asyncio.sleep(TIMEOUT)
            if not check_running_pid(pid):
                return True

        print(f"Scenario {k} (pid: {pid}) is still running. Try to kill it")
        os.kill(pid, 15)    # SIGTERM
        await asyncio.sleep(1)
        if not check_running_pid(pid):
            return True
        print(f"Scenario {k} (pid: {pid}) is still running. Try to kill it by force")
        os.kill(pid, 9)    # SIGKILL
        await asyncio.sleep(1)
        if not check_running_pid(pid):
            return True

        print(f"Scenario {k} (pid: {pid}) failed to terminate. You have to manually end it")
        return False
