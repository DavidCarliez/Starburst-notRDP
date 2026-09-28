from mythic_container.MythicCommandBase import *
from mythic_container.MythicRPC import *


class NotRdpArguments(TaskArguments):
    def __init__(self, command_line, **kwargs):
        super().__init__(command_line, **kwargs)
        self.args = [
            CommandParameter(
                name="action",
                cli_name="Action",
                display_name="Action",
                type=ParameterType.ChooseOne,
                choices=["start", "shot", "input", "live", "stop"],
                default_value="start",
                description="start: create hidden desktop; shot: capture screen; input: inject mouse/keyboard; live: reverse-connect to browser viewer; stop: teardown",
                parameter_group_info=[
                    ParameterGroupInfo(
                        required=True,
                        group_name="Default",
                        ui_position=1,
                    )
                ],
            ),
            CommandParameter(
                name="x",
                cli_name="X",
                display_name="Mouse X",
                type=ParameterType.Number,
                default_value=0,
                description="Mouse X coordinate (for input action only)",
                parameter_group_info=[
                    ParameterGroupInfo(
                        required=False,
                        group_name="Default",
                        ui_position=2,
                    )
                ],
            ),
            CommandParameter(
                name="y",
                cli_name="Y",
                display_name="Mouse Y",
                type=ParameterType.Number,
                default_value=0,
                description="Mouse Y coordinate (for input action only)",
                parameter_group_info=[
                    ParameterGroupInfo(
                        required=False,
                        group_name="Default",
                        ui_position=3,
                    )
                ],
            ),
            CommandParameter(
                name="mouse_action",
                cli_name="MouseAction",
                display_name="Mouse/Key Action",
                type=ParameterType.Number,
                default_value=0,
                description="0=move,1=lclick,2=rclick,3=dblclick,4=ldown,5=lup,6=rdown,7=rup,8=scroll,10=kpress,11=kdown,12=kup",
                parameter_group_info=[
                    ParameterGroupInfo(
                        required=False,
                        group_name="Default",
                        ui_position=4,
                    )
                ],
            ),
            CommandParameter(
                name="key",
                cli_name="Key",
                display_name="Virtual Key Code",
                type=ParameterType.Number,
                default_value=0,
                description="Virtual key code for keyboard input (e.g. 0x41 for 'A', 0x0D for Enter)",
                parameter_group_info=[
                    ParameterGroupInfo(
                        required=False,
                        group_name="Default",
                        ui_position=5,
                    )
                ],
            ),
            CommandParameter(
                name="host",
                cli_name="Host",
                display_name="Viewer Host",
                type=ParameterType.String,
                default_value="",
                description="Operator IP for live streaming (live action only)",
                parameter_group_info=[
                    ParameterGroupInfo(
                        required=False,
                        group_name="Default",
                        ui_position=6,
                    )
                ],
            ),
            CommandParameter(
                name="port",
                cli_name="Port",
                display_name="Viewer Port",
                type=ParameterType.Number,
                default_value=8124,
                description="Operator viewer agent-port (live action only)",
                parameter_group_info=[
                    ParameterGroupInfo(
                        required=False,
                        group_name="Default",
                        ui_position=7,
                    )
                ],
            ),
            CommandParameter(
                name="fps",
                cli_name="Fps",
                display_name="Frames per second",
                type=ParameterType.Number,
                default_value=2,
                description="JPEG frames per second (live action only)",
                parameter_group_info=[
                    ParameterGroupInfo(
                        required=False,
                        group_name="Default",
                        ui_position=8,
                    )
                ],
            ),
            CommandParameter(
                name="scale",
                cli_name="Scale",
                display_name="Resolution divisor",
                type=ParameterType.Number,
                default_value=1,
                description="Output divisor from 1 (full resolution) through 4",
                parameter_group_info=[
                    ParameterGroupInfo(
                        required=False,
                        group_name="Default",
                        ui_position=9,
                    )
                ],
            ),
        ]

    async def parse_arguments(self):
        if len(self.command_line) == 0:
            self.add_arg("action", "start")
            return
        if self.command_line[0] == "{":
            self.load_args_from_json_string(self.command_line)
        else:
            # Simple string: "start" or "shot" or "stop"
            parts = self.command_line.strip().split()
            if parts and parts[0] in ("start", "shot", "stop"):
                self.add_arg("action", parts[0])
            elif parts and parts[0] == "input" and len(parts) >= 3:
                self.add_arg("action", "input")
                self.add_arg("x", int(parts[1]))
                self.add_arg("y", int(parts[2]))
                if len(parts) >= 4:
                    self.add_arg("mouse_action", int(parts[3]))
                if len(parts) >= 5:
                    self.add_arg("key", int(parts[4]))
            elif parts and parts[0] == "live":
                self.add_arg("action", "live")
                if len(parts) >= 2 and parts[1].lower() == "stop":
                    self.add_arg("host", "stop")
                    return
                if len(parts) >= 2:
                    self.add_arg("host", parts[1])
                if len(parts) >= 3:
                    self.add_arg("port", int(parts[2]))
                if len(parts) >= 4:
                    self.add_arg("fps", int(parts[3]))
                if len(parts) >= 5:
                    self.add_arg("scale", int(parts[4]))
            else:
                self.add_arg("action", self.command_line.strip())


class NotRdpCommand(CommandBase):
    cmd = "notrdp"
    needs_admin = False
    help_cmd = (
        "notrdp [start|shot|stop] | notrdp input <x> <y> [action] [key] | "
        "notrdp live <ip> <port> [fps] [scale] | notrdp live stop"
    )
    description = (
        "Alternate Windows desktop with tasked BMP capture and input, plus "
        "interactive JPEG streaming to the browser viewer over reverse TCP. "
        "The agent must run in a user session because session 0 desktops do "
        "not render. Use the default sleep mask while live streaming."
    )
    version = 2
    supported_ui_features = []
    author = "@operator"
    attackmapping = ["T1113", "T1562.001"]
    argument_class = NotRdpArguments
    attributes = CommandAttributes(
        builtin=False,
        supported_os=[SupportedOS.Windows],
    )

    async def create_go_tasking(self, taskData: MythicCommandBase.PTTaskMessageAllData) -> MythicCommandBase.PTTaskCreateTaskingMessageResponse:
        response = MythicCommandBase.PTTaskCreateTaskingMessageResponse(
            TaskID=taskData.Task.ID, Success=True,
        )
        action = taskData.args.get_arg("action")
        if action == "start":
            response.DisplayParams = "-Action start"
        elif action == "shot":
            response.DisplayParams = "-Action shot"
        elif action == "stop":
            response.DisplayParams = "-Action stop"
        elif action == "input":
            x = taskData.args.get_arg("x") or 0
            y = taskData.args.get_arg("y") or 0
            ma = taskData.args.get_arg("mouse_action") or 0
            key = taskData.args.get_arg("key") or 0
            response.DisplayParams = f"-Action input ({x},{y}) action={ma} key={key}"
        elif action == "live":
            host = taskData.args.get_arg("host") or ""
            port = taskData.args.get_arg("port") or 8124
            fps = taskData.args.get_arg("fps") or 2
            scale = taskData.args.get_arg("scale") or 1
            response.DisplayParams = f"-Action live {host}:{port} fps={fps} scale={scale}"
        return response

    async def process_response(self, task: PTTaskMessageAllData, response: any) -> PTTaskProcessResponseMessageResponse:
        return PTTaskProcessResponseMessageResponse(TaskID=task.Task.ID, Success=True)
