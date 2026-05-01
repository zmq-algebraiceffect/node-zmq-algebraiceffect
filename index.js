const path = require('path');

const addon = require(path.join(__dirname, 'build', 'Release', 'zmq-algebraiceffect-node.node'));

module.exports = {
    Client: addon.Client,
    Router: addon.Router,
    _shutdown: addon._shutdown,
};
