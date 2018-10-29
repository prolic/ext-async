--TEST--
UDP cancel receive operation.
--SKIPIF--
<?php
if (!extension_loaded('task')) echo 'Test requires the task extension to be loaded';
?>
--FILE--
<?php

namespace Concurrent\Network;

use Concurrent\Context;
use Concurrent\Task;

$t = Task::asyncWithContext(Context::current()->withTimeout(500), function () {
    $socket = UdpSocket::bind('127.0.0.1', 0);
    
    var_dump('RECEIVING...');
    
    try {
        return $socket->receive();
    } catch (\Throwable $e) {
        var_dump($e->getMessage());
        
        throw $e;
    }
});

try {
    Task::await($t);
} catch (\Throwable $e) {
    var_dump($e->getMessage());
}

--EXPECT--
string(12) "RECEIVING..."
string(17) "Context timed out"
string(17) "Context timed out"
