import os

class Process:
    def __init__(self, pid=None):
        self.pid = pid if pid is not None else os.getpid()
    
    @property
    def value(self):
        return self.pid
    
    def __getattr__(self, name):
        # Mock other methods/properties if needed
        if name == 'parent':
            return lambda: Process(1)
        if name == 'name':
            return lambda: 'cmake'
        return lambda *args, **kwargs: None

def get_cmake_pid():
    return os.getpid()

# Mock other psutil functions used by the component manager
def NoSuchProcess(*args, **kwargs):
    return Exception("NoSuchProcess")

def AccessDenied(*args, **kwargs):
    return Exception("AccessDenied")
