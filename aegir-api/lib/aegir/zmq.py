'''
ZMQ handling, takes care of the per-flask-instance initialization, and
provides a highlevel communication API for the app
'''

import zmq

import aegir.config

_ctx = None
_socket_pr = None

def init(app):
    app.before_first_request(instance_init)
    pass

def instance_init():
    global _ctx, _socket_pr

    _ctx = zmq.Context()
    _socket_pr = _ctx.socket(zmq.REQ)
    _socket_pr.connect('tcp://127.0.0.1:{port}'.format(port = aegir.config.config['prport']))
    pass

def prmessage(command, data):
    global _socket_pr

    msg = {'command': command,
           'data': data}
    _socket_pr.send_json(msg)
    return _socket_pr.recv_json()