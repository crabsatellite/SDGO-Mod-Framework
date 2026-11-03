"""Bounded retry for Windows readers that briefly deny rename/delete sharing."""
import time


def retry_sharing(action, timeout=1.0):
    deadline=time.monotonic()+timeout
    while True:
        try:return action()
        except PermissionError:
            if time.monotonic()>=deadline:raise
            time.sleep(.01)
