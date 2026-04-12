"""
Tests the Frame specific Lua libraries over Bluetooth.
"""

import asyncio
from frameutils import Bluetooth


async def main():
    b = Bluetooth()

    await b.connect(print_response_handler=lambda s: print(s))

    await b.send_lua("frame.experiment.hello_world_quant()")

    await asyncio.sleep(1)

    await b.disconnect()


asyncio.run(main())
