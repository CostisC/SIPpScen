#!/usr/bin/env python3

import yaml, argparse, subprocess, atexit
import asyncio
import sys, shutil, math, copy, os, time
from jinja2 import Environment, FileSystemLoader, exceptions as jinjaExcept
import sippscen_utils as u


SIPP = "sipp"
CONTROL_PORT = 8888
INTERCALL_GAP_SEC = 2   # leave a gap before reusing the same caller DN
REGISTR_MIN_SEC = 300   # the min registrar expire value
REFER_PAUSE_SEC = 4     # secs after which a refer is sent

PORTS_TEMPLATE = \
"""SEQUENTIAL,PRINTF={nPorts},PRINTFMULTIPLE=2,PRINTFOFFSET={offset}
%d
"""

DN_TEMPLATE = \
"""SEQUENTIAL,PRINTF={nPorts},PRINTFOFFSET={offset}
{prefix}%0{length}d
"""


async def terminate(scenario: dict, type: str = None) -> bool:
    """
    Asynchonously terminate scenarios, based on type.
    Normally, clients should terminate before servers.
    """
    if type:
        scn = {k: v for k,v in scenario.items() if v["type"] == type}
    else:
        scn = scenario
    return await asyncio.gather(
            *(u.stop_scenario({k: v}) for k,v in scn.items())
            )


def add_optional_arg(flag:str, value:any) -> list:
    """
    if value is not None, add it as a CLI flag
    """
    if value:
        return [flag, str(value)]
    else:
        return []


def compose_cli(
        type: str,
        logpath: str,
        xml_file: str,
        csv_file: list,
        local_ip: str,
        sip_port: int,
        call_service: str = None,
        receive_tm: int = None,    # timeout in ms
        send_tm: int = None,       # timeout in ms
        cps: int = None,
        destination: str = None,
        total_calls: int = None,
        control_port: int = None,
        background: bool = True,
        ) -> str:

    cli = [SIPP]
    cli += ["-t", "t1" ]
    if background:
        cli.append("-bg")
    cli += add_optional_arg("-recv_timeout", receive_tm)
    cli += add_optional_arg("-send_timeout", send_tm)
    cli += ["-trace_err", "-error_file"]
    cli.append(f"{logpath}_errors.out")
    cli += ["-trace_msg", "-rfc3339", "-message_file"]
    cli.append(f"{logpath}_traces.out")
    cli += ["-sf", xml_file]
    for infile in csv_file:
        cli += ["-inf", infile]
    # cli += ["-ringbuffer_size", str(5_000_000)]
    # cli += ["-ringbuffer_files", '1']
    cli += ["-p", str(sip_port)]
    cli += add_optional_arg("-s", call_service)
    cli += add_optional_arg("-cp", control_port)
    cli += ["-i", local_ip]

    match type:
        case 'client':
            cli += add_optional_arg("-m", total_calls)
            cli += ["-r", str(cps)]
            if not destination:
                raise Exception("Missing destination")
            cli.append(destination)
            # TODO: configurable report freq
            cli += ["-trace_stat", "-fd", "5", "-stf"]
            cli.append(f"{logpath}_stats.csv")
        case 'server':
            pass
        case _:
            print(f"Unknown type '{scn_data['type']}'")
            return ""

    return cli


def parse_args():
    parser = argparse.ArgumentParser(description =
        """ SIPp scenarios orchestrator
        """,
        formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument('-d', '--dry-run', action="store_true",
        help = "Run in dry run mode")
    parser.add_argument('-c', '--config', required=True,
        help = "Configuration YAML file ")

    return parser.parse_args()


# =~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~
#                        MAIN
# =~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~=~

if __name__ == "__main__":

    argv = sys.argv[1:]
    bDry_run = False if ('-d' not in argv) else True

    args = parse_args()
    bDry_run = args.dry_run

    def dprint(msg: str):
        if bDry_run:
            print(msg)

    #DIR = os.path.dirname(__file__)
    CWD = os.path.dirname(os.path.abspath(args.config))

    try:
        with open(args.config, 'r') as file:
            config = yaml.safe_load(file)
    except yaml.scanner.ScannerError as err:
        print(f"ERROR: yaml parsing\n {err}")
        sys.exit(1)

    u.title("Check Media Servers availability")
    media_servers = u.check_server(config["rtp-servers"])
    port_ranges = {k:[] for k in media_servers.keys()}


    OUTPUT_DIR = os.path.join(CWD, "run_conf")
    SCENARIOS_DIR = os.path.join(CWD, "scenarios")
    LOGS_DIR = os.path.join(CWD, "logs")
    u.mkdir(OUTPUT_DIR)
    u.rm_files(OUTPUT_DIR)
    u.mkdir(LOGS_DIR)
    get_control_port = u.get_next_port(CONTROL_PORT)
    dScenarios = dict()
    dRegistrations = dict()
    dUnregistrations = dict()
    sip_sockets = list()

    run_duration = u.translate_time_signature(
        config.get("run").get("duration")
    )

    g_receive_timeout = config.get("run").get("receive-timeout")
    g_send_timeout = config.get("run").get("send-timeout")


    u.title("Prepare Scenarios")
    environment = Environment(loader=FileSystemLoader(SCENARIOS_DIR))
    for scenario in config.get("scenario"):
        for scn_name, scn_data in scenario.items():
            try:
                print(f"\n=~=~=~=~  {scn_name}  ~=~=~=~=")
                conf = dict()
                # If rtp-server is set, check first that it is responsive
                if ms := scn_data.get("rtp-server"):
                    if ms not in media_servers:
                        print(f"{ms} is unresponsive. I will skip this test")
                        continue
                    conf['media_server'] = media_servers[ms]
                    media_ip = media_servers[ms].split(':')[0]
                    conf['media_ip'] = media_ip


                # Create the call scenario instructions
                template_name = scn_data["scenario-file"]
                template = environment.get_template(template_name)
                out_path = os.path.join(OUTPUT_DIR, scn_name)
                csv_file_ports = out_path + "_ports.csv"
                csv_file_dn = out_path + "_dn.csv"

                call_duration = u.translate_time_signature(str(scn_data["call-duration"]))
                conf['duration'] = call_duration      # media-server duration is in ms
                conf['ports_file'] = os.path.basename(csv_file_ports)
                conf['dn_file'] = os.path.basename(csv_file_dn)
                conf['refer_pause'] = REFER_PAUSE_SEC * 1_000
                conf['bye_pause'] = call_duration - conf['refer_pause'] - 1_000
                call_duration = int(call_duration/1_000)
                content = template.render(conf)
                xml_file = out_path + ".xml"
                with open(xml_file, 'w') as file:
                    file.write(content)

                # Create the RTP ports description
                n_ports = scn_data.get("number-endpoints") or \
                    math.ceil(scn_data["cps"] * (call_duration + INTERCALL_GAP_SEC))
                if n_calls := scn_data.get("total-calls"):
                    if n_calls < n_ports:
                        n_ports = n_calls
                ports_offset = scn_data["rtp-port-offset"]
                port_ranges[ms].append((ports_offset, (ports_offset+2*n_ports)-1, scn_name))

                ports_template = PORTS_TEMPLATE.format(nPorts=n_ports, offset=ports_offset)

                with open(csv_file_ports, 'w') as file:
                    file.write(ports_template)

                # The DN template
                dn_start = scn_data["dn-start"]
                (str_prefix, d_suffix) = u.dn_prefix(dn_start, n_ports)

                dn_template = DN_TEMPLATE.format(nPorts=n_ports,
                                                 offset=int(str(dn_start)[-d_suffix:]),
                                                 prefix= str_prefix,
                                                 length=d_suffix)

                with open(csv_file_dn, 'w') as file:
                    file.write(dn_template)
                print(f"{n_ports} DNs: [{dn_start}, {dn_start+n_ports-1}]")

                # Compose the sipp command arguments
                control_port = get_control_port()
                local_ip = scn_data.get("local-ip", u.get_primary_ip())
                sip_sockets.append(local_ip + ":" + str(scn_data["sip-port"]))

                receive_tm = u.translate_time_signature(d) \
                    if (d := scn_data.get("receive-timeout", g_receive_timeout)) else None

                send_tm = u.translate_time_signature(d) \
                    if (d := scn_data.get("send-timeout", g_send_timeout)) else None


                args = {
                    'logpath': os.path.join(LOGS_DIR, scn_name),
                    'type': scn_data["type"],
                    'xml_file': xml_file,
                    'csv_file': [csv_file_ports, csv_file_dn],
                    'local_ip': local_ip,
                    'sip_port': scn_data["sip-port"],
                    'call_service': scn_data.get("call-service"),
                    'receive_tm': receive_tm,
                    'send_tm': send_tm,
                    'cps': scn_data["cps"],
                    'destination': scn_data.get("destination"),
                    'total_calls': scn_data.get("total-calls"),
                    'control_port': control_port,
                }

                cli = compose_cli(**args)


                # Create the Registration instructions
                if "register" in scn_data:
                    dprint(f"-~-> Register:")
                    reg = scn_data["register"]
                    template_name = reg["register-file"]
                    template = environment.get_template(template_name)
                    conf = dict()
                    conf['reg_expire_sec'] = int(run_duration / 1_000) + REGISTR_MIN_SEC
                    xml_file = os.path.join(OUTPUT_DIR, f"{scn_name}_register.xml")
                    content = template.render(conf)
                    with open(xml_file, 'w') as file:
                        file.write(content)

                    args = {
                        'logpath': os.path.join(LOGS_DIR,f"{scn_name}_register"),
                        'type': 'client',
                        'xml_file': xml_file,
                        'csv_file': [csv_file_dn],
                        'local_ip': local_ip,
                        'sip_port': scn_data["sip-port"],
                        'receive_tm': receive_tm,
                        'send_tm': send_tm,
                        'cps': reg.get("cps", 10),      # rate of registering DNs - default: 10 per sec
                        'destination': reg["registrar"],
                        'total_calls': n_ports,
                        'background': False,
                    }

                    reg_cli = compose_cli(**args)
                    dprint(" ".join(reg_cli))
                    dRegistrations[scn_name] = {"cli": reg_cli, "dn-range": (dn_start, dn_start+n_ports)}

                    if template_name := reg.get("unregister-file"):
                        xml_file = os.path.join(OUTPUT_DIR, f"{scn_name}_unregister.xml")
                        shutil.copy(os.path.join(SCENARIOS_DIR, template_name), xml_file)
                        args['logpath'] = os.path.join(LOGS_DIR,f"{scn_name}_unregister")
                        args['xml_file'] = xml_file
                        dUnregistrations[scn_name] = compose_cli(**args)


                dprint(f"-~-> Run:")
                dprint(" ".join(cli))
                dScenarios[scn_name] = {"cPort": control_port, "pid": None, "ip": local_ip,
                                        "type": scn_data['type'], "duration": call_duration, "cli": cli}
                if cli := dUnregistrations.get(scn_name):
                    dprint(f"-~-> Unregister:")
                    dprint(" ".join(cli))


                # Scenarios that define a pattern of cps changes
                if pattern := scn_data.get("pattern"):
                    pattern["timepoints"] = [u.translate_time_signature(i) for i in pattern["timepoints"]]
                    if len(pattern["timepoints"]) != len(pattern["cps"]):
                        print("ERROR: pattern timepoints vs cps count mismatch")
                    else:
                        dScenarios[scn_name]["pattern"] = pattern

            except jinjaExcept.TemplateNotFound as err:
                print(f"ERROR: Non-existing template: {err}")
            except KeyError as err:
                print(f"ERROR: {err} not defined")
            except Exception as err:
                print(f"ERROR: {err}")

    # validate that there are no overlapping media ports
    u.title("Validate no ports overlap")
    if u.check_overlap(port_ranges) or not u.check_no_duplicate(sip_sockets):
        sys.exit(1)


    print()
    if bDry_run or len(dScenarios) == 0:
        sys.exit(0)

    def close_telegraf(pid):
        time.sleep(5)
        pid.kill()
        pid.wait(5)

    # start telegraf
    p_telegraf = None
    if telegraf := config.get("influxdb"):
        if t_path := telegraf.get("telegraf_fullpath"):
            telegraf["tag"] = telegraf.get("tag", "default")
            telegraf["logspath"] = LOGS_DIR
            p_telegraf = subprocess.Popen([t_path, "--config",
                                           "/etc/media-server/SIPpScen/telegraf.conf"],
                                           env = telegraf,
                                           stderr=subprocess.PIPE)
            time.sleep(.5)
            if p_telegraf.poll():
                print("ERROR: Failed to start telegraf")
                sys.exit(1)
            atexit.register(close_telegraf, p_telegraf)


    u.title("Start Scenarios", True)
    for reg,config in dRegistrations.items():
       print(f"-~- Registering {reg}")
       rc = u.spawn_process(config["cli"], background=False)
       range = config["dn-range"]
       if rc == 0 or rc == 99:
           print(f"DNs {range} successfully registered")
       else:
           print(f"ERROR: registering {range}")
           sys.exit(1)

    b_running = False
    for scenario in dScenarios:
       print(f"-~- {scenario}")
       scn_data = dScenarios[scenario]
       try:
           if p := u.spawn_process(scn_data["cli"]):
               print(f"Process spawned as {p}")
               scn_data["pid"] = p
               b_running = True
           else:
               raise subprocess.SubprocessError("Failed to start up.")

       except Exception as err:
           print(err)

    end_time_ms = run_duration + u.get_epoch_ms()

    scn_w_pattern = {k:v["pattern"] for k,v in dScenarios.items() if v.get("pattern")}
    pattern_streams = dict()

    while b_running and (cur_time_ms := u.get_epoch_ms()) < end_time_ms:
        for k,v in scn_w_pattern.items():
            # if a non-repeated-scenario is already passed or a scenario has remaining timepoints,
            # skip their timepoint recalculation
            if "passed" in v or ( k in pattern_streams and pattern_streams[k]['streams'] ):
                continue
            t = v["timepoints"]
            streams = [t[0]]
            for x in t[1:]:
                streams.append(x + streams[-1])
            cps_list = copy.copy(v['cps'])
            pattern_streams[k] = {'streams': list(map(lambda x: cur_time_ms+x, streams)),
                                  'cps': cps_list}

        # which pattern scenario is to change next:
        scn, config = min(pattern_streams.items(), default=(None,None), key=lambda x: x[1]['streams'][0])
        if scn:
            next_timepoint = config['streams'].pop(0)
            cps = config['cps'].pop(0)
            wait_time_s = (next_timepoint - cur_time_ms) / 1_000
            if wait_time_s < 0:
                wait_time_s = 0
            time.sleep(wait_time_s)
            u.title(f"{scn} cps changed to {cps}", True, True)
            u.send_control_cmd("setRate", dScenarios[scn]['cPort'], dScenarios[scn]['ip'], cps)
            if len(config['streams']) == 0 and not scn_w_pattern[scn].get('repeat'):
                scn_w_pattern[scn]['passed'] = True
                pattern_streams.pop(scn)
        else:
            wait_time_s = (end_time_ms - cur_time_ms) / 1_000
            time.sleep(wait_time_s)



    u.title("stop clients", True)
    asyncio.run(terminate(dScenarios, type="client"))
    u.title("stop servers", True)
    asyncio.run(terminate(dScenarios, type="server"))

    if len(dUnregistrations):
        u.title(f"-~- Unregistering devices", True)
    for reg,cli in dUnregistrations.items():
       rc = u.spawn_process(cli, background=False)
       if rc == 0 or rc == 99:
           print(f"{reg}: Successfully unregistered")
       else:
           print(f"ERROR: unregistering {reg}")

    u.title("Close", True)

