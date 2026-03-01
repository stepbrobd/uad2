{
  name = "uad2";

  interactive.sshBackdoor.enable = true;

  nodes.machine.imports = [ ./module.nix ];

  testScript = ''
    machine.succeed("lsmod | grep uad2")
    machine.succeed("modinfo uad2 | grep 'description:.*Universal Audio'")

    machine.succeed("modprobe -r uad2")
    machine.fail("lsmod | grep uad2")

    machine.succeed("modprobe uad2")
    machine.succeed("lsmod | grep uad2")
  '';
}
