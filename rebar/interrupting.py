import signal
from .contextlib import maybeasynccontextmanager
import aljpy

log = aljpy.logger()

class Interrupter:

    def __init__(self):
        self._is_set = False

    def check(self):
        if self._is_set:
            self.reset()
            raise KeyboardInterrupt()

    def handle(self, signum, frame):
        log.info('Setting interrupt flag')
        self._is_set = True

    def reset(self):
        self._is_set = False

_INTERRUPTER = Interrupter()

@maybeasynccontextmanager
def interrupter():
    old = signal.signal(signal.SIGINT, _INTERRUPTER.handle)
    try:
        yield _INTERRUPTER
    finally:
        signal.signal(signal.SIGINT, old)