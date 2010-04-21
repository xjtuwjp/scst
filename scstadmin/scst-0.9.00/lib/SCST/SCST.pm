package SCST::SCST;

# Author:	Mark R. Buechler
# License:	GPLv2
# Copyright (c) 2005-2010 Mark R. Buechler

use 5.005;
use Fcntl ':mode';
use IO::Handle;
use IO::File;
use strict;
use Carp qw(cluck);

use constant {
TRUE             => 1,
FALSE            => 0,

SCST_ROOT        => '/sys/kernel/scst_tgt',

# Root-level
SCST_HANDLERS    => 'handlers',
SCST_DEVICES     => 'devices',
SCST_TARGETS     => 'targets',
SCST_SGV         => 'sgv',

# Target specific
SCST_GROUPS      => 'ini_groups',
SCST_INITIATORS  => 'initiators',
SCST_SESSIONS    => 'sessions',
SCST_LUNS        => 'luns',

# Files
SCST_MGMT_IO     => 'mgmt',
SCST_VERSION_IO  => 'version',
SCST_TRACE_IO    => 'trace_level',
SCST_RESYNC_IO   => 'resync_size',
SCST_T10_IO      => 't10_dev_id',

# Module return codes
SCST_C_FATAL_ERROR          => 2,

SCST_C_HND_NO_HANDLER       => 10,

SCST_C_DEV_NO_DEVICE        => 20,
SCST_C_DEV_EXISTS           => 21,
SCST_C_DEV_OPEN_FAIL        => 22,
SCST_C_DEV_CLOSE_FAIL       => 23,
SCST_C_DEV_BAD_PARAMETERS   => 27,
SCST_C_DEV_PARAMETER_STATIC => 28,
SCST_C_DEV_SETPARAM_FAIL    => 29,

SCST_C_DRV_NO_DRIVER        => 30,
SCST_C_DRV_BAD_PARAMETERS   => 37,
SCST_C_DRV_PARAMETER_STATIC => 38,
SCST_C_DRV_SETPARAM_FAIL    => 39,

SCST_C_TGT_NO_TARGET        => 40,
SCST_C_TGT_BAD_PARAMETERS   => 47,
SCST_C_TGT_PARAMETER_STATIC => 48,
SCST_C_TGT_SETPARAM_FAIL    => 49,

SCST_C_GRP_NO_GROUP         => 50,
SCST_C_GRP_EXISTS           => 51,
SCST_C_GRP_ADD_FAIL         => 52,
SCST_C_GRP_REM_FAIL         => 53,

SCST_C_GRP_NO_LUN           => 60,
SCST_C_GRP_LUN_EXISTS       => 61,
SCST_C_GRP_ADD_LUN_FAIL     => 62,
SCST_C_GRP_REM_LUN_FAIL     => 63,
SCST_C_GRP_CLR_LUN_FAIL     => 65,

SCST_C_GRP_NO_INI           => 70,
SCST_C_GRP_INI_EXISTS       => 71,
SCST_C_GRP_ADD_INI_FAIL     => 72,
SCST_C_GRP_REM_INI_FAIL     => 73,
SCST_C_GRP_MOV_INI_FAIL     => 74,
SCST_C_GRP_CLR_INI_FAIL     => 75,

SCST_C_LUN_DEV_EXISTS       => 81,
SCST_C_LUN_RPL_DEV_FAIL     => 86,
SCST_C_LUN_BAD_PARAMETERS   => 87,
SCST_C_LUN_PARAMETER_STATIC => 88,
SCST_C_LUN_SETPARAM_FAIL    => 89,
};

use vars qw(@ISA @EXPORT $VERSION);

$VERSION = 0.9.00;

my $_SCST_MIN_MAJOR_   = 2;
my $_SCST_MIN_MINOR_   = 0;
my $_SCST_MIN_RELEASE_ = 0;

sub new {
	my $this = shift;
	my $debug = shift;
	my $badVersion = 1;
	
	my $class = ref($this) || $this;
	my $self = {};

	bless($self, $class);

	$self->{'debug'} = $debug;

	my $scstVersion = $self->scstVersion();

	die("Failed to obtain SCST version information. Is the scst module loaded?\n")
	  if ($scstVersion == -1);

	my($major, $minor, $release) = split(/\./, $scstVersion, 3);
	($release, undef) = split(/\-/, $release) if ($release =~ /\-/);

	$badVersion = 0 if (($major > $_SCST_MIN_MAJOR_) ||
			    (($major == $_SCST_MIN_MAJOR_) && ($minor > $_SCST_MIN_MINOR_)) ||
			    (($major == $_SCST_MIN_MAJOR_) && ($minor == $_SCST_MIN_MINOR_) &&
			     ($release >= $_SCST_MIN_RELEASE_)));

	die("This module requires at least SCST version $_SCST_MIN_MAJOR_\.$_SCST_MIN_MINOR_\.".
	      "$_SCST_MIN_RELEASE_ and version $scstVersion was found") if ($badVersion);

	return $self;
}

sub scstVersion {
	my $self = shift;

	my $_path = mkpath(SCST_ROOT, SCST_VERSION_IO);
	my $io = new IO::File $_path, O_RDONLY;
	if (!defined($io)) {
		$self->{'err_string'} = "version(): Failed to open '$_path' for reading: $!";
		return undef;
	}

	my $version = <$io>; # Only want first line
	chomp $version;

	return $version;
}

sub drivers {
	my $self = shift;
	my @drivers;

	my $dHandle = new IO::Handle;
	my $_path = mkpath(SCST_ROOT, SCST_TARGETS);
	if (!(opendir $dHandle, $_path)) {
		$self->{'err_string'} = "drivers(): Unable to read directory '$_path': $!";
		return undef;
	}

	foreach my $driver (readdir($dHandle)) {
		next if (($driver eq '.') || ($driver eq '..'));

		if (-d mkpath(SCST_ROOT, SCST_TARGETS, $driver)) {
			push @drivers, $driver;
		}
	}

	close $dHandle;

	return \@drivers;
}


sub targets {
	my $self = shift;
	my $driver = shift;
	my @targets;

	if ($self->driverExists($driver) != TRUE) {
		$self->{'err_string'} = "targets(): Driver '$driver' is not available";
		return undef;
	}

	my $tHandle = new IO::Handle;
	my $_path = mkpath(SCST_ROOT, SCST_TARGETS, $driver);
	if (!(opendir $tHandle, $_path)) {
		$self->{'err_string'} = "targets(): Unable to read directory '$_path': $!";
		return undef;
	}

	foreach my $target (readdir($tHandle)) {
		next if (($target eq '.') || ($target eq '..'));

		if (-d mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target)) {
			push @targets, $target;
		}
	}

	close $tHandle;

	return \@targets;
}

sub groups {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my @groups;

	if ($self->driverExists($driver) != TRUE) {
		$self->{'err_string'} = "groups(): Driver '$driver' is not available";
		return undef;
	}

	if ($self->targetExists($driver, $target) != TRUE) {
		$self->{'err_string'} = "groups(): Target '$target' is not available";
		return undef;
	}

	my $gHandle = new IO::Handle;
	my $_path = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target, SCST_GROUPS);
	if (!(opendir $gHandle, $_path)) {
		$self->{'err_string'} = "groups(): Unable to read directory '$_path': $!";
		return undef;
	}

	foreach my $group (readdir($gHandle)) {
		next if (($group eq '.') || ($group eq '..'));

		if (-d mkpath(SCST_ROOT, SCST_TARGETS, $driver,
		  $target, SCST_GROUPS, $group)) {
			push @groups, $group;
		}
	}

	close $gHandle;

	return \@groups;
}

sub initiators {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my $group = shift;
	my @initiators;

	if ($self->driverExists($driver) != TRUE) {
		$self->{'err_string'} = "groups(): Driver '$driver' is not available";
		return undef;
	}

	if ($self->targetExists($driver, $target) != TRUE) {
		$self->{'err_string'} = "groups(): Target '$target' is not available";
		return undef;
	}

	if ($self->groupExists($driver, $target, $group) != TRUE) {
		$self->{'err_string'} = "initiators(): Group '$group' does not exists";
		return undef;
	}

	my $iHandle = new IO::Handle;
	my $_path = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target, SCST_GROUPS,
	  $group, SCST_INITIATORS);
	if (!(opendir $iHandle, $_path)) {
		$self->{'err_string'} = "initiators(): Unable to read directory '$_path': $!";
		return undef;
	}

	foreach my $initiator (readdir($iHandle)) {
		next if (($initiator eq '.') || ($initiator eq '..'));
		next if ($initiator eq SCST_MGMT_IO);

		push @initiators, $initiator;
	}

	close $iHandle;

	return \@initiators;
}

sub luns {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my $group = shift;
	my %luns;

	if ($self->driverExists($driver) != TRUE) {
		$self->{'err_string'} = "groups(): Driver '$driver' is not available";
		return undef;
	}

	if ($self->targetExists($driver, $target) != TRUE) {
		$self->{'err_string'} = "groups(): Target '$target' is not available";
		return undef;
	}

	if ($self->groupExists($driver, $target, $group) != TRUE) {
		$self->{'err_string'} = "initiators(): Group '$group' does not exists";
		return undef;
	}

	my $lHandle = new IO::Handle;
	my $_path = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target, SCST_GROUPS,
	  $group, SCST_LUNS);
	if (!(opendir $lHandle, $_path)) {
		$self->{'err_string'} = "luns(): Unable to read directory '$_path': $!";
		return undef;
	}

	foreach my $lun (readdir($lHandle)) {
		next if (($lun eq '.') || ($lun eq '..'));

		if (-d mkpath(SCST_ROOT, SCST_TARGETS, $driver,
		  $target, SCST_GROUPS, $group, SCST_LUNS, $lun)) {
			my $lPath = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target,
			  SCST_GROUPS, $group, SCST_LUNS, $lun);

			my $_lHandle = new IO::Handle;

			if (!(opendir $_lHandle, $lPath)) {
				$self->{'err_string'} = "luns(): Unable to read directory '$lPath': $!";
				return undef;
			}

			foreach my $parameter (readdir($_lHandle)) {
				my $pPath = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target,
				  SCST_GROUPS, $group, SCST_LUNS, $lun, $parameter);

				if (-l $pPath) {
					my $linked = readlink($pPath);

					my $d = SCST_DEVICES;

					if ($linked =~ /.*\/$d\/(.*)/) {
						$luns{$lun} = $1;
					}
				}
			}
		}
	}

	close $lHandle;

	return \%luns;
}

sub driverExists {
	my $self = shift;
	my $driver = shift;

	my $drivers = $self->drivers();

	return SCST_C_FATAL_ERROR if (!defined($drivers));

	foreach my $_driver (@{$drivers}) {
		return TRUE if ($driver eq $_driver);
	}

	return FALSE;
}

sub targetExists {
	my $self = shift;
	my $driver = shift;
	my $target = shift;

	return FALSE if (!defined($target));

	my $rc = $self->driverExists($driver);

	return FALSE if (!$rc);
	return $rc if ($rc > 1);

	my $targets = $self->targets($driver);

	return SCST_C_FATAL_ERROR if (!defined($targets));

	foreach my $_target (@{$targets}) {
		return TRUE if ($target eq $_target);
	}

	return FALSE;
}

sub groupExists {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my $group = shift;

	return FALSE if (!defined($group));

	my $rc = $self->targetExists($driver, $target);

	return FALSE if (!$rc);
	return $rc if ($rc > 1);

	my $groups = $self->groups($driver, $target);

	return SCST_C_FATAL_ERROR if (!defined($groups));

	foreach my $_group (@{$groups}) {
		return TRUE if ($group eq $_group);
	}

	return FALSE;
}

sub initiatorExists {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my $group = shift;
	my $initiator = shift;

	return FALSE if (!defined($initiator));

	my $rc = $self->groupExists($driver, $target, $group);

	return FALSE if (!$rc);
	return $rc if ($rc > 1);

	my $initiators = $self->initiators($driver, $target, $group);

	return SCST_C_FATAL_ERROR if (!defined($initiators));

	foreach my $_initiator (@{$initiators}) {
		return TRUE if ($initiator eq $_initiator);
	}

	return FALSE;
}

sub lunExists {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my $group = shift;
	my $lun = shift;

	return FALSE if (!defined($lun));

	my $rc = $self->groupExists($driver, $target, $group);

	return FALSE if (!$rc);
	return $rc if ($rc > 1);

	my $luns = $self->luns($driver, $target, $group);

	return SCST_C_FATAL_ERROR if (!defined($luns));

	foreach my $_lun (keys %{$luns}) {
		return TRUE if ($lun == $_lun);
	}

	return FALSE;
}

sub addGroup {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my $group = shift;

	my $rc = $self->driverExists($driver);
	return SCST_C_DRV_NO_DRIVER if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->targetExists($driver, $target);
	return SCST_C_TGT_NO_TARGET if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->groupExists($driver, $target, $group);
	return SCST_C_GRP_EXISTS if ($rc == TRUE);
	return $rc if ($rc > 1);

	my $path = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target,
	  SCST_GROUPS, SCST_MGMT_IO);

	my $io = new IO::File $path, O_WRONLY;

	return SCST_C_GRP_ADD_FAIL if (!$io);

	my $cmd = "create $group\n";
	my $bytes;

	if ($self->{'debug'}) {
		print "DBG($$): $path -> $cmd\n";
	} else {
		$bytes = syswrite($io, $cmd, length($cmd));
	}

	close $io;

	return FALSE if ($self->{'debug'} || $bytes);
	return SCST_C_GRP_ADD_FAIL;
}

sub removeGroup {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my $group = shift;

	my $rc = $self->driverExists($driver);
	return SCST_C_DRV_NO_DRIVER if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->targetExists($driver, $target);
	return SCST_C_TGT_NO_TARGET if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->groupExists($driver, $target, $group);
	return SCST_C_GRP_NO_GROUP if (!$rc);
	return $rc if ($rc > 1);

	my $path = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target,
	  SCST_GROUPS, SCST_MGMT_IO);

	my $io = new IO::File $path, O_WRONLY;

	return SCST_C_GRP_REM_FAIL if (!$io);

	my $cmd = "del $group\n";
	my $bytes;

	if ($self->{'debug'}) {
		print "DBG($$): $path -> $cmd\n";
	} else {
		$bytes = syswrite($io, $cmd, length($cmd));
	}

	close $io;

	return FALSE if ($self->{'debug'} || $bytes);
	return SCST_C_GRP_REM_FAIL;
}

sub addInitiator {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my $group = shift;
	my $initiator = shift;

	my $rc = $self->driverExists($driver);
	return SCST_C_DRV_NO_DRIVER if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->targetExists($driver, $target);
	return SCST_C_TGT_NO_TARGET if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->groupExists($driver, $target, $group);
	return SCST_C_GRP_NO_GROUP if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->initiatorExists($driver, $target, $group, $initiator);
	return SCST_C_GRP_INI_EXISTS if ($rc == TRUE);
	return $rc if ($rc > 1);

	my $path = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target, SCST_GROUPS,
	  $group, SCST_INITIATORS, SCST_MGMT_IO);

	my $io = new IO::File $path, O_WRONLY;

	return SCST_C_GRP_ADD_INI_FAIL if (!$io);

	my $cmd = "add $initiator\n";
	my $bytes;

	if ($self->{'debug'}) {
		print "DBG($$): $path -> $cmd\n";
	} else {
		$bytes = syswrite($io, $cmd, length($cmd));
	}

	close $io;

	return FALSE if ($self->{'debug'} || $bytes);
	return SCST_C_GRP_ADD_INI_FAIL;
}

sub removeInitiator {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my $group = shift;
	my $initiator = shift;

	my $rc = $self->driverExists($driver);
	return SCST_C_DRV_NO_DRIVER if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->targetExists($driver, $target);
	return SCST_C_TGT_NO_TARGET if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->groupExists($driver, $target, $group);
	return SCST_C_GRP_NO_GROUP if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->initiatorExists($driver, $target, $group, $initiator);
	return SCST_C_GRP_NO_INI if (!$rc);
	return $rc if ($rc > 1);

	my $path = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target, SCST_GROUPS,
	  $group, SCST_INITIATORS, SCST_MGMT_IO);

	my $io = new IO::File $path, O_WRONLY;

	return SCST_C_GRP_REM_INI_FAIL if (!$io);

	my $cmd = "del $initiator\n";
	my $bytes;

	if ($self->{'debug'}) {
		print "DBG($$): $path -> $cmd\n";
	} else {
		$bytes = syswrite($io, $cmd, length($cmd));
	}

	close $io;

	return FALSE if ($self->{'debug'} || $bytes);
	return SCST_C_GRP_REM_INI_FAIL;
}

sub moveInitiator {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my $from = shift;
	my $to = shift;
	my $initiator = shift;

	my $rc = $self->driverExists($driver);
	return SCST_C_DRV_NO_DRIVER if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->targetExists($driver, $target);
	return SCST_C_TGT_NO_TARGET if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->groupExists($driver, $target, $from);
	return SCST_C_GRP_NO_GROUP if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->groupExists($driver, $target, $to);
	return SCST_C_GRP_NO_GROUP if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->initiatorExists($driver, $target, $from, $initiator);
	return SCST_C_GRP_NO_INI if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->initiatorExists($driver, $target, $to, $initiator);
	return SCST_C_GRP_INI_EXISTS if ($rc == TRUE);
	return $rc if ($rc > 1);

	my $path = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target, SCST_GROUPS,
	  $from, SCST_INITIATORS, SCST_MGMT_IO);

	my $io = new IO::File $path, O_WRONLY;

	return SCST_C_GRP_MOV_INI_FAIL if (!$io);

	my $cmd = "move $initiator $to\n";
	my $bytes;

	if ($self->{'debug'}) {
		print "DBG($$): $path -> $cmd\n";
	} else {
		$bytes = syswrite($io, $cmd, length($cmd));
	}

	close $io;

	return FALSE if ($self->{'debug'} || $bytes);
	return SCST_C_GRP_MOV_INI_FAIL;
}

sub clearInitiators {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my $group = shift;

	my $rc = $self->driverExists($driver);
	return SCST_C_DRV_NO_DRIVER if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->targetExists($driver, $target);
	return SCST_C_TGT_NO_TARGET if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->groupExists($driver, $target, $group);
	return SCST_C_GRP_NO_GROUP if (!$rc);
	return $rc if ($rc > 1);

	my $path = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target, SCST_GROUPS,
	  $group, SCST_INITIATORS, SCST_MGMT_IO);

	my $io = new IO::File $path, O_WRONLY;

	return SCST_C_GRP_CLR_INI_FAIL if (!$io);

	my $cmd = "clear\n";
	my $bytes;

	if ($self->{'debug'}) {
		print "DBG($$): $path -> $cmd\n";
	} else {
		$bytes = syswrite($io, $cmd, length($cmd));
	}

	close $io;

	return FALSE if ($self->{'debug'} || $bytes);
	return SCST_C_GRP_CLR_INI_FAIL;
}

sub addLun {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my $group = shift;
	my $device = shift;
	my $lun = shift;
	my $parameters = shift;

	return SCST_C_GRP_ADD_LUN_FAIL if (!defined($lun));

	my $rc = $self->driverExists($driver);
	return SCST_C_DRV_NO_DRIVER if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->targetExists($driver, $target);
	return SCST_C_TGT_NO_TARGET if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->groupExists($driver, $target, $group);
	return SCST_C_GRP_NO_GROUP if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->lunExists($driver, $target, $group, $lun);
	return SCST_C_GRP_LUN_EXISTS if ($rc == TRUE);
	return $rc if ($rc > 1);

	$rc = $self->deviceOpen($device);
	return SCST_C_DEV_NO_DEVICE if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->checkLunAvailableParameters($driver, $target, $group, $parameters);
	return SCST_C_LUN_BAD_PARAMETERS if ($rc == TRUE);
	return $rc if ($rc > 1);

	my $path = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target, SCST_GROUPS,
	  $group, SCST_LUNS, SCST_MGMT_IO);

	my $io = new IO::File $path, O_WRONLY;

	return SCST_C_GRP_ADD_LUN_FAIL if (!$io);

	my $o_string;
	foreach my $parameter (keys %{$parameters}) {
		my $value = $$parameters{$parameter};
		$o_string .= "$parameter=$value; ";
	}

	$o_string =~ s/\s$//;
	my $cmd = "add $device $lun $o_string\n";

	my $bytes;

	if ($self->{'debug'}) {
		print "DBG($$): $path -> $cmd\n";
	} else {
		$bytes = syswrite($io, $cmd, length($cmd));
	}

	close $io;

	return FALSE if ($self->{'debug'} || $bytes);
	return SCST_C_GRP_ADD_LUN_FAIL;
}

sub removeLun {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my $group = shift;
	my $device = shift;
	my $lun = shift;

	my $rc = $self->driverExists($driver);
	return SCST_C_DRV_NO_DRIVER if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->targetExists($driver, $target);
	return SCST_C_TGT_NO_TARGET if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->groupExists($driver, $target, $group);
	return SCST_C_GRP_NO_GROUP if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->lunExists($driver, $target, $group, $lun);
	return SCST_C_GRP_NO_LUN if (!$rc);
	return $rc if ($rc > 1);

	my $luns = $self->luns($driver, $target, $group);

	return SCST_C_GRP_REM_LUN_FAIL if ($$luns{$lun} ne $device);

	my $path = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target, SCST_GROUPS,
	  $group, SCST_LUNS, SCST_MGMT_IO);

	my $io = new IO::File $path, O_WRONLY;

	return SCST_C_GRP_REM_LUN_FAIL if (!$io);

	my $cmd = "del $device $lun\n";
	my $bytes;

	if ($self->{'debug'}) {
		print "DBG($$): $path -> $cmd\n";
	} else {
		$bytes = syswrite($io, $cmd, length($cmd));
	}

	close $io;

	return FALSE if ($self->{'debug'} || $bytes);
	return SCST_C_GRP_REM_LUN_FAIL;
}

sub replaceLun {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my $group = shift;
	my $lun = shift;
	my $device = shift;
	my $parameters = shift;

	return TRUE if (!defined($lun));

	my $rc = $self->driverExists($driver);
	return SCST_C_DRV_NO_DRIVER if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->targetExists($driver, $target);
	return SCST_C_TGT_NO_TARGET if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->groupExists($driver, $target, $group);
	return SCST_C_GRP_NO_GROUP if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->lunExists($driver, $target, $group, $lun);
	return SCST_C_GRP_NO_LUN if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->deviceOpen($device);
	return SCST_C_DEV_NO_DEVICE if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->checkLunAvailableParameters($driver, $target, $group, $parameters);
	return SCST_C_LUN_BAD_PARAMETERS if ($rc == TRUE);
	return $rc if ($rc > 1);

	my $luns = $self->luns($driver, $target, $group);

	return SCST_C_LUN_DEV_EXISTS if ($$luns{$lun} eq $device);

	my $path = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target, SCST_GROUPS,
	  $group, SCST_LUNS, SCST_MGMT_IO);

	my $io = new IO::File $path, O_WRONLY;

	return SCST_C_LUN_RPL_DEV_FAIL if (!$io);

	my $o_string;
	foreach my $parameter (keys %{$parameters}) {
		my $value = $$parameters{$parameter};
		$o_string .= "$parameter=$value; ";
	}

	$o_string =~ s/\s$//;
	my $cmd = "replace $device $lun $o_string\n";
	my $bytes;

	if ($self->{'debug'}) {
		print "DBG($$): $path -> $cmd\n";
	} else {
		$bytes = syswrite($io, $cmd, length($cmd));
	}

	close $io;

	return FALSE if ($self->{'debug'} || $bytes);
	return SCST_C_LUN_RPL_DEV_FAIL;
}

sub clearLuns {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my $group = shift;

	my $rc = $self->driverExists($driver);
	return SCST_C_DRV_NO_DRIVER if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->targetExists($driver, $target);
	return SCST_C_TGT_NO_TARGET if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->groupExists($driver, $target, $group);
	return SCST_C_GRP_NO_GROUP if (!$rc);
	return $rc if ($rc > 1);

	my $path = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target, SCST_GROUPS,
	  $group, SCST_LUNS, SCST_MGMT_IO);

	my $io = new IO::File $path, O_WRONLY;

	return SCST_C_GRP_CLR_LUN_FAIL if (!$io);

	my $cmd = "clear\n";
	my $bytes;

	if ($self->{'debug'}) {
		print "DBG($$): $path -> $cmd\n";
	} else {
		$bytes = syswrite($io, $cmd, length($cmd));
	}

	close $io;

	return FALSE if ($self->{'debug'} || $bytes);
	return SCST_C_GRP_CLR_LUN_FAIL;
}

sub devices {
	my $self = shift;
	my @devices;

	my $dHandle = new IO::Handle;
	my $_path = mkpath(SCST_ROOT, SCST_DEVICES);
	if (!(opendir $dHandle, $_path)) {
		$self->{'err_string'} = "devices(): Unable to read directory '$_path': $!";
		return undef;
	}

	foreach my $device (readdir($dHandle)) {
		next if (($device eq '.') || ($device eq '..'));

                if (-d mkpath(SCST_ROOT, SCST_DEVICES, $device)) {
			push @devices, $device;
		}							
	}

	close $dHandle;

	return \@devices;
}

sub deviceOpen {
	my $self = shift;
	my $device = shift;

	return FALSE if (!defined($device));

	my $devices = $self->devices();

	return SCST_C_FATAL_ERROR if (!defined($devices));

	foreach my $_device (@{$devices}) {
		return TRUE if ($device eq $_device);
	}

	return FALSE;
}

sub deviceParameters {
	my $self = shift;
	my $device = shift;
	my %parameters;

	if ($self->deviceOpen($device) != TRUE) {
		$self->{'err_string'} = "deviceParameters(): Device '$device' is not open";
		return undef;
	}

	my $pHandle = new IO::Handle;	
	my $_path = mkpath(SCST_ROOT, SCST_DEVICES, $device); 
	if (!(opendir $pHandle, $_path)) {
		$self->{'err_string'} = "deviceParameters(): Unable to read directory '$_path': $!";
		return undef;
	}

	foreach my $parameter (readdir($pHandle)) {
		next if (($parameter eq '.') || ($parameter eq '..'));
		my $pPath = mkpath(SCST_ROOT, SCST_DEVICES, $device, $parameter);
		my $mode = (stat($pPath))[2];

		if ($parameter eq 'exported') {
			my $eHandle = new IO::Handle;
			opendir $eHandle, mkpath(SCST_ROOT, SCST_DEVICES,
			  $device, $parameter);

			foreach my $export (readdir($eHandle)) {
				next if (($export eq '.') || ($export eq '..'));

				my $linked = readlink mkpath($pPath, $export);

				my $t = SCST_TARGETS;
				my $g = SCST_GROUPS;
				my $l = SCST_LUNS;

				if ($linked =~ /\.\.\/\.\.\/$t\/(.+)\/(.+)\/$g\/(.+)\/$l\/(\d+)/) {
					my $driver = $1;
					my $target = $2;
					my $group = $3;
					my $lun = $4;

					$parameters{$parameter}->{$driver}->{$target}->{$group} = $lun;
				}
			}
		} elsif ($parameter eq 'handler') {
			my $linked = readlink $pPath;

			my $h = SCST_HANDLERS;

			if ($linked =~ /\.\.\/\.\.\/$h\/(.*)/) {
				my $handler = $1;
				$parameters{$parameter}->{'static'} = TRUE;
				$parameters{$parameter}->{'value'} = $handler;
			}
		} else {
			if (!(($mode & S_IRUSR) >> 6)) {
				$parameters{$parameter}->{'static'} = FALSE;
				$parameters{$parameter}->{'value'} = undef;
			} else {
				my $is_static;
				if (($mode & S_IWUSR) >> 6) {
					$is_static = FALSE;
				} else {
					$is_static = TRUE;
				}

				my $io = new IO::File $pPath, O_RDONLY;

				if (!$io) {
					$self->{'err_string'} = "deviceParameters(): Unable to read ".
					  "device parameter '$parameter': $!";
					return undef;
				}

				my $value = <$io>;
				chomp $value;

				if ($parameter eq 'type') {
					my($type, $type_string) = split(/\s\-\s/, $value, 2);
					$parameters{$parameter}->{'value'} = $type;
					$parameters{'type_string'}->{'value'} = $type_string;
					$parameters{'type_string'}->{'static'} = TRUE;
				} else {
					$parameters{$parameter}->{'value'} = $value;
				}

				$parameters{$parameter}->{'static'} = $is_static;
			}
		}
	}

	return \%parameters;
}

sub driverParameters {
	my $self = shift;
	my $driver = shift;
	my %parameters;

	if ($self->driverExists($driver) != TRUE) {
		$self->{'err_string'} = "driverParameters(): Driver '$driver' is not available";
		return undef;
	}

	my $pHandle = new IO::Handle;	
	my $_path = mkpath(SCST_ROOT, SCST_TARGETS, $driver); 
	if (!(opendir $pHandle, $_path)) {
		$self->{'err_string'} = "driverParameters(): Unable to read directory '$_path': $!";
		return undef;
	}

	foreach my $parameter (readdir($pHandle)) {
		next if (($parameter eq '.') || ($parameter eq '..'));
		my $pPath = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $parameter);
		my $mode = (stat($pPath))[2];

		if (-d $pPath) {
			# Skip directories
		} else {
			if (!(($mode & S_IRUSR) >> 6)) {
				$parameters{$parameter}->{'static'} = FALSE;
				$parameters{$parameter}->{'value'} = undef;
			} else {
				my $is_static;
				if (($mode & S_IWUSR) >> 6) {
					$is_static = FALSE;
				} else {
					$is_static = TRUE;
				}

				my $io = new IO::File $pPath, O_RDONLY;

				if (!$io) {
					$self->{'err_string'} = "driverParameters(): Unable to read ".
					  "driver parameter '$parameter': $!";
					return undef;
				}

				my $value = <$io>;
				chomp $value;

				if ($parameter eq SCST_TRACE_IO) {
					$parameters{$parameter}->{'value'} = '';
					my @possible;
					foreach my $t (split(/\|/, $value)) {
						$t =~ s/^\s//; $t =~ s/\s$//;
						push @possible, $t;
					}
					$parameters{$parameter}->{'set'} = \@possible;
				} else {
					$parameters{$parameter}->{'value'} = $value;
				}

				$parameters{$parameter}->{'static'} = $is_static;
			}
		}
	}

	return \%parameters;
}

sub setDriverParameter {
	my $self = shift;
	my $driver = shift;
	my $parameter = shift;
	my $value = shift;

	my $rc = $self->driverExists($driver);
	return SCST_C_DRV_NO_DRIVER if (!$rc);
	return $rc if ($rc > 1);

	return TRUE if (!defined($parameter) || !defined($value));

	my $parameters = $self->driverParameters($driver);

	return SCST_C_DRV_BAD_PARAMETERS if (!defined($$parameters{$parameter}));
	return SCST_C_DRV_PARAMETER_STATIC if ($$parameters{$parameter}->{'static'});

	my $path = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $parameter);

	my $io = new IO::File $path, O_WRONLY;

	return SCST_C_DRV_SETPARAM_FAIL if (!$io);

	my $bytes;

	if ($self->{'debug'}) {
		print "DBG($$): $path -> $parameter = $value\n";
	} else {
		$bytes = syswrite($io, $value, length($value));
	}

	close $io;

	return FALSE if ($self->{'debug'} || $bytes);
        return SCST_C_DRV_SETPARAM_FAIL;
}

sub targetParameters {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my %parameters;

	if ($self->driverExists($driver) != TRUE) {
		$self->{'err_string'} = "targetParameters(): Driver '$driver' is not available";
		return undef;
	}

	if ($self->targetExists($driver, $target) != TRUE) {
		$self->{'err_string'} = "targetParameters(): Target '$target' is not available";
		return undef;
	}

	my $pHandle = new IO::Handle;	
	my $_path = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target); 
	if (!(opendir $pHandle, $_path)) {
		$self->{'err_string'} = "targetParameters(): Unable to read directory '$_path': $!";
		return undef;
	}

	foreach my $parameter (readdir($pHandle)) {
		next if (($parameter eq '.') || ($parameter eq '..'));
		my $pPath = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target, $parameter);
		my $mode = (stat($pPath))[2];

		if ($parameter eq 'host') {
			my $linked = readlink($pPath);

			$linked =~ s/\.\.\/\.\.\/\.\.\/\.\.\/\.\.\//\/sys\//;

			$parameters{$parameter}->{'static'} = TRUE;
			$parameters{$parameter}->{'value'} = $linked;
		} elsif (-d $pPath) {
			# Skip directories
		} else {
			if (!(($mode & S_IRUSR) >> 6)) {
				$parameters{$parameter}->{'static'} = FALSE;
				$parameters{$parameter}->{'value'} = undef;
			} else {
				my $is_static;
				if (($mode & S_IWUSR) >> 6) {
					$is_static = FALSE;
				} else {
					$is_static = TRUE;
				}

				my $io = new IO::File $pPath, O_RDONLY;

				if (!$io) {
					$self->{'err_string'} = "targetParameters(): Unable to read ".
					  "target parameter '$parameter': $!";
					return undef;
				}

				my $value = <$io>;
				chomp $value;

				$parameters{$parameter}->{'static'} = $is_static;
				$parameters{$parameter}->{'value'} = $value;
			}
		}
	}

	return \%parameters;
}

sub setTargetParameter {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my $parameter = shift;
	my $value = shift;

	my $rc = $self->driverExists($driver);
	return SCST_C_DRV_NO_DRIVER if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->targetExists($driver, $target);
	return SCST_C_TGT_NO_TARGET if (!$rc);
	return $rc if ($rc > 1);

	return TRUE if (!defined($parameter) || !defined($value));

	my $parameters = $self->targetParameters($driver, $target);

	return SCST_C_TGT_BAD_PARAMETERS if (!defined($$parameters{$parameter}));
	return SCST_C_TGT_PARAMETER_STATIC if ($$parameters{$parameter}->{'static'});

	my $path = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target, $parameter);

	my $io = new IO::File $path, O_WRONLY;

	return SCST_C_TGT_SETPARAM_FAIL if (!$io);

	my $bytes;

	if ($self->{'debug'}) {
		print "DBG($$): $path -> $parameter = $value\n";
	} else {
		$bytes = syswrite($io, $value, length($value));
	}

	close $io;

	return FALSE if ($self->{'debug'} || $bytes);
        return SCST_C_TGT_SETPARAM_FAIL;
}

sub lunParameters {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my $group = shift;
	my $lun = shift;
	my %parameters;

	if ($self->driverExists($driver) != TRUE) {
		$self->{'err_string'} = "lunParameters(): Driver '$driver' is not available";
		return undef;
	}

	if ($self->targetExists($driver, $target) != TRUE) {
		$self->{'err_string'} = "lunParameters(): Target '$target' is not available";
		return undef;
	}

	if ($self->groupExists($driver, $target, $group) != TRUE) {
		$self->{'err_string'} = "lunParameters(): Group '$group' does not exists";
		return undef;
	}

	if ($self->lunExists($driver, $target, $group, $lun) != TRUE) {
		$self->{'err_string'} = "lunParameters(): LUN '$lun' does not exist";
		return undef;
	}

	my $pHandle = new IO::Handle;	
	my $_path = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target, SCST_GROUPS,
	  $group, SCST_LUNS, $lun); 
	if (!(opendir $pHandle, $_path)) {
		$self->{'err_string'} = "lunParameters(): Unable to read directory '$_path': $!";
		return undef;
	}

	foreach my $parameter (readdir($pHandle)) {
		next if (($parameter eq '.') || ($parameter eq '..'));
		my $pPath = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target, SCST_GROUPS,
		  $group, SCST_LUNS, $lun, $parameter);
		my $mode = (stat($pPath))[2];

		if ($parameter eq 'device') {
			my $linked = readlink($pPath);

			my $r = SCST_ROOT;

			$linked =~ s/\.\.\/\.\.\/\.\.\/\.\.\/\.\.\/\.\.\/\.\.\//\/$r\//;

			$parameters{$parameter}->{'static'} = TRUE;
			$parameters{$parameter}->{'value'} = $linked;
		} elsif (-d $pPath) {
			# Skip directories
		} else {
			if (!(($mode & S_IRUSR) >> 6)) {
				$parameters{$parameter}->{'static'} = FALSE;
				$parameters{$parameter}->{'value'} = undef;
			} else {
				my $is_static;
				if (($mode & S_IWUSR) >> 6) {
					$is_static = FALSE;
				} else {
					$is_static = TRUE;
				}

				my $io = new IO::File $pPath, O_RDONLY;

				if (!$io) {
					$self->{'err_string'} = "lunParameters(): Unable to read ".
					  "lun parameter '$parameter': $!";
					return undef;
				}

				my $value = <$io>;
				chomp $value;

				$parameters{$parameter}->{'static'} = $is_static;
				$parameters{$parameter}->{'value'} = $value;
			}
		}
	}

	return \%parameters;
}

sub setLunParameter {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my $group = shift;
	my $lun = shift;
	my $parameter = shift;
	my $value = shift;

	my $rc = $self->driverExists($driver);
	return SCST_C_DRV_NO_DRIVER if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->targetExists($driver, $target);
	return SCST_C_TGT_NO_TARGET if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->groupExists($driver, $target, $group);
	return SCST_C_GRP_NO_GROUP if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->lunExists($driver, $target, $group, $lun);
	return SCST_C_GRP_NO_LUN if (!$rc);
	return $rc if ($rc > 1);

	return TRUE if (!defined($parameter) || !defined($value));

	my $parameters = $self->lunParameters($driver, $target, $group, $lun);

	return SCST_C_LUN_BAD_PARAMETERS if (!defined($$parameters{$parameter}));
	return SCST_C_LUN_PARAMETER_STATIC if ($$parameters{$parameter}->{'static'});

	my $path = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target, SCST_GROUPS,
	   $group, SCST_LUNS, $lun, $parameter);

	my $io = new IO::File $path, O_WRONLY;

	return SCST_C_LUN_SETPARAM_FAIL if (!$io);

	my $bytes;

	if ($self->{'debug'}) {
		print "DBG($$): $path -> $parameter = $value\n";
	} else {
		$bytes = syswrite($io, $value, length($value));
	}

	close $io;

	return FALSE if ($self->{'debug'} || $bytes);
        return SCST_C_LUN_SETPARAM_FAIL;
}

sub handlers {
	my $self = shift;
	my @handlers;

	my $hHandle = new IO::Handle;
	my $_path = mkpath(SCST_ROOT, SCST_HANDLERS);
	if (!(opendir $hHandle, $_path)) {
		$self->{'err_string'} = "handlers(): Unable to read directory '$_path': $!";
		return undef;
	}

	foreach my $handler (readdir($hHandle)) {
		next if (($handler eq '.') || ($handler eq '..'));

		if (-d mkpath(SCST_ROOT, SCST_HANDLERS)) {
			push @handlers, $handler;
		}
	}

	close $hHandle;

	return \@handlers;
}

sub handlerExists {
	my $self = shift;
	my $handler = shift;

	return FALSE if (!defined($handler));

	my $handlers = $self->handlers();

	return SCST_C_FATAL_ERROR if (!defined($handlers));

	foreach my $_handler (@{$handlers}) {
		return TRUE if ($handler eq $_handler);
	}

	return FALSE;
}

sub handlerParameters {
	my $self = shift;
	my $handler = shift;
	my %parameters;

	if ($self->handlerExists($handler) != TRUE) {
		$self->{'err_string'} = "handlerParameters(): Handler '$handler' is not available";
		return undef;
	}

	my $hHandle = new IO::Handle;
	my $_path = mkpath(SCST_ROOT, SCST_HANDLERS, $handler);
	if (!(opendir $hHandle, $_path)) {
		$self->{'err_string'} = "handlerParameters(): Unable to read directory '$_path': $!";
		return undef;
	}

	foreach my $parameter (readdir($hHandle)) {
		next if (($parameter eq '.') || ($parameter eq '..'));
		next if ($parameter eq SCST_MGMT_IO);
		my $pPath = mkpath(SCST_ROOT, SCST_HANDLERS, $handler, $parameter);
		my $mode = (stat($pPath))[2];

		if (-d $pPath) {
			push @{$parameters{'devices'}}, $parameter;
			next;
		}

		my $is_static;
		if (($mode & S_IWUSR) >> 6) {
			$is_static = FALSE;
		} else {
			$is_static = TRUE;
		}

		my $io = new IO::File
		  mkpath(SCST_ROOT, SCST_HANDLERS, $handler, $parameter), O_RDONLY;

		if (!$io) {
			$self->{'err_string'} = "handlerParameters(): Unable to read handler parameter ".
			  "'$parameter': $!";
			return undef;
		}

		my $value = <$io>;
		chomp $value;

		if (!(($mode & S_IRUSR) >> 6)) {
			$parameters{$parameter}->{'static'} = FALSE;
			$parameters{$parameter}->{'value'} = undef;
		} elsif ($parameter eq SCST_TRACE_IO) {
			$parameters{$parameter}->{'value'} = '';
			my @possible;
			foreach my $t (split(/\|/, $value)) {
				$t =~ s/^\s//; $t =~ s/\s$//;
				push @possible, $t;
			}
			$parameters{$parameter}->{'set'} = \@possible;
		} elsif ($parameter eq 'type') {
			my($type, $type_string) = split(/\s\-\s/, $value, 2);
			$parameters{$parameter}->{'value'} = $type;
			$parameters{'type_string'}->{'value'} = $type_string;
			$parameters{'type_string'}->{'static'} = TRUE;
		} else {
			$parameters{$parameter}->{'value'} = $value;
		}

		$parameters{$parameter}->{'static'} = $is_static;

		close $io;
	}

	return \%parameters;
}

sub handlerDeviceExists {
	my $self = shift;
	my $handler = shift;
	my $device = shift;

	my $rc = $self->handlerExists($handler);

	return FALSE if (!$rc);
	return $rc if ($rc > 1);

	my $devices = $self->devicesByHandler($handler);

	return SCST_C_FATAL_ERROR if (!defined($devices));

	foreach my $_device (@{$devices}) {
		return TRUE if ($_device eq $device);
	}

	return FALSE;
}

sub devicesByHandler {
	my $self = shift;
	my $handler = shift;

	if ($self->handlerExists($handler) != TRUE) {
		$self->{'err_string'} = "devicesByHandler(): Handler '$handler' is not available";
		return undef;
	}

	my $parameters = $self->handlerParameters($handler);

	return undef if (!defined($parameters));
	return \@{$$parameters{'devices'}};
}

sub checkDeviceAvailableParameters {
	my $self = shift;
	my $handler = shift;
	my $check = shift;

	return FALSE if (!defined($check));

	my $rc = $self->handlerExists($handler);
	return SCST_C_HND_NO_HANDLER if (!$rc);
	return $rc if (!$rc > 1);

	my $available = $self->deviceAvailableParameters($handler);

	return SCST_C_FATAL_ERROR if (!defined($available));

	foreach my $parameter (keys %{$check}) {
		if (!defined($$available{$parameter})) {
			return TRUE;
		}
	}

	return FALSE;
}

sub deviceAvailableParameters {
	my $self = shift;
	my $handler = shift;
	my $available;
	my %parameters;

	if ($self->handlerExists($handler) != TRUE) {
		$self->{'err_string'} = "deviceAvailableParameters():Handler '$handler' ".
		  "is not available";
		return undef;
	}

	my $io = new IO::File mkpath(SCST_ROOT, SCST_HANDLERS, $handler, SCST_MGMT_IO), O_RDONLY;

	if (!$io) {
		$self->{'err_string'} = "deviceAvailableParameters(): Unable to open mgmt ".
		  "interface for handler '$handler': $!";
		return undef;
	}

	while (my $in = <$io>) {
		if ($in =~ /^The following parameters available\:/) {
			(undef, $available) = split(/\:/, $in, 2);
			$available =~ s/\.$//;
		}
	}

	if ($available) {
		foreach my $parameter (split(/\,/, $available)) {
			$parameter =~ s/^\s+//;
			$parameter =~ s/\s+$//;
			$parameters{$parameter} = '';
		}
	}

	return \%parameters;
}

sub openDevice {
	my $self = shift;
	my $handler = shift;
	my $device = shift;
	my $parameters = shift;

	my $rc = $self->handlerExists($handler);
	return SCST_C_HND_NO_HANDLER if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->checkDeviceAvailableParameters($handler, $parameters);
	return SCST_C_DEV_BAD_PARAMETERS if ($rc == TRUE);
	return $rc if ($rc > 1);

	my $io = new IO::File mkpath(SCST_ROOT, SCST_HANDLERS, $handler, SCST_MGMT_IO), O_WRONLY;

	if (!$io) {
		$self->{'err_string'} = "openDevice(): Unable to open mgmt interface for ".
		  "handler '$handler': $!";
		return SCST_C_FATAL_ERROR;
	}

	$rc = $self->handlerDeviceExists($handler, $device);
	return SCST_C_DEV_EXISTS if ($rc == TRUE);
	return $rc if ($rc > 1);

	my $o_string;
	foreach my $parameter (keys %{$parameters}) {
		my $value = $$parameters{$parameter};
		$o_string .= "$parameter=$value; ";
	}

	$o_string =~ s/\s$//;
	my $cmd = "add_device $device $o_string\n";

	my $bytes;

        if ($self->{'debug'}) {                
		print "DBG($$): $cmd\n";
	} else {
		$bytes = syswrite($io, $cmd, length($cmd));
	}

        return FALSE if ($self->{'debug'} || $bytes);
	return SCST_C_DEV_OPEN_FAIL;
}

sub closeDevice {
	my $self = shift;
	my $handler = shift;
	my $device = shift;

	my $rc = $self->handlerExists($handler);
	return SCST_C_HND_NO_HANDLER if (!$rc);
	return $rc if ($rc > 1);

	my $io = new IO::File mkpath(SCST_ROOT, SCST_HANDLERS, $handler, SCST_MGMT_IO), O_WRONLY;

	if (!$io) {
		$self->{'err_string'} = "closeDevice(): Unable to open mgmt interface for handler ".
		  "'$handler': $!";
		return SCST_C_FATAL_ERROR;
	}

	$rc = $self->handlerDeviceExists($handler, $device);
	return SCST_C_DEV_NO_DEVICE if ($rc == TRUE);
	return $rc if ($rc > 1);

	my $cmd = "del_device $device\n";

	my $bytes;

        if ($self->{'debug'}) {                
		print "DBG($$): $cmd\n";
	} else {
		$bytes = syswrite($io, $cmd, length($cmd));
	}

        return FALSE if ($self->{'debug'} || $bytes);
	return SCST_C_DEV_CLOSE_FAIL;
}

sub setDeviceParameter {
	my $self = shift;
	my $device = shift;
	my $parameter = shift;
	my $value = shift;

	my $rc = $self->deviceOpen($device);
	return SCST_C_DEV_NO_DEVICE if (!$rc);
	return $rc if ($rc > 1);

	my $parameters = $self->deviceParameters($device);

	return SCST_C_FATAL_ERROR if (!defined($parameters));

	return SCST_C_DEV_BAD_PARAMETERS if (!defined($$parameters{$parameter}));
	return SCST_C_DEV_PARAMETER_STATIC if ($$parameters{$parameter}->{'static'});

	my $path = mkpath(SCST_ROOT, SCST_DEVICES, $device, $parameter);

	my $io = new IO::File $path, O_WRONLY;

	return SCST_C_DEV_SETPARAM_FAIL if (!$io);

	my $bytes;

	if ($self->{'debug'}) {
		print "DBG($$): $path -> $parameter = $value\n";
	} else {
		$bytes = syswrite($io, $value, length($value));
	}

	close $io;

	return FALSE if ($self->{'debug'} || $bytes);
        return SCST_C_DEV_SETPARAM_FAIL;
}

sub enableTarget {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my $enable = shift;

	$enable = TRUE if ($enable);

	return $self->setTargetParameter($driver, $target, 'enabled', $enable);
}

sub resyncDevice {
	my $self = shift;
	my $device = shift;

	return $self->setDeviceParameter($device, 'resync_size', 1);
}

sub setT10DeviceId {
	my $self = shift;
	my $device = shift;
	my $t10_id = shift;

	return $self->setDeviceParameter($device, 't10_dev_id', $t10_id);
}

sub checkLunAvailableParameters {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my $group = shift;
	my $check = shift;

	return FALSE if (!defined($check));

	my $rc = $self->driverExists($driver);
	return SCST_C_DRV_NO_DRIVER if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->targetExists($driver, $target);
	return SCST_C_TGT_NO_TARGET if (!$rc);
	return $rc if ($rc > 1);

	$rc = $self->groupExists($driver, $target, $group);
	return SCST_C_GRP_NO_GROUP if (!$rc);
	return $rc if ($rc > 1);

	my $available = $self->lunAvailableParameters($driver, $target, $group);

	return SCST_C_FATAL_ERROR if (!defined($available));

	foreach my $parameter (keys %{$check}) {
		if (!defined($$available{$parameter})) {
			return TRUE;
		}
	}

	return FALSE;
}

sub lunAvailableParameters {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my $group = shift;
	my $available;
	my %parameters;

	if ($self->driverExists($driver) != TRUE) {
		$self->{'err_string'} = "lunAvailableParameters(): Driver '$driver' ".
		  "is not available";
		return undef;
	}

	if ($self->targetExists($driver, $target) != TRUE) {
		$self->{'err_string'} = "lunAvailableParameters(): Target '$target' ".
		  "is not available";
		return undef;
	}

	if ($self->groupExists($driver, $target, $group) != TRUE) {
		$self->{'err_string'} = "lunAvailableParameters(): Group '$group' ".
		  "does not exist";
		return undef;
	}

	my $io = new IO::File mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target,
	  SCST_GROUPS, $group, SCST_LUNS, SCST_MGMT_IO), O_RDONLY;

	if (!$io) {
		$self->{'err_string'} = "lunAvailableParameters(): Unable to open luns mgmt ".
		  "interface for group '$group': $!";
		return undef;
	}

	while (my $in = <$io>) {
		if ($in =~ /^The following parameters available\:/) {
			(undef, $available) = split(/\:/, $in, 2);
			$available =~ s/\.$//;
		}
	}

	if ($available) {
		foreach my $parameter (split(/\,/, $available)) {
			$parameter =~ s/^\s+//;
			$parameter =~ s/\s+$//;
			$parameters{$parameter} = '';
		}
	}

	return \%parameters;
}

sub sessions {
	my $self = shift;
	my $driver = shift;
	my $target = shift;
	my %_sessions;

	if ($self->driverExists($driver) != TRUE) {
		$self->{'err_string'} = "sessions(): Driver '$driver' ".
		  "is not available";
		return undef;
	}

	if ($self->targetExists($driver, $target) != TRUE) {
		$self->{'err_string'} = "sessions(): Target '$target' ".
		  "is not available";
		return undef;
	}

	my $sHandle = new IO::Handle;	
	my $_path = mkpath(SCST_ROOT, SCST_TARGETS, $driver, $target, SCST_SESSIONS); 
	if (!(opendir $sHandle, $_path)) {
		$self->{'err_string'} = "sessions(): Unable to read directory '$_path': $!";
		return undef;
	}

	foreach my $session (readdir($sHandle)) {
		next if (($session eq '.') || ($session eq '..'));
		my $pHandle = new IO::Handle;
		my $sPath = mkpath($_path, $session);
		if (!(opendir $pHandle, $sPath)) {
			$self->{'err_string'} = "sessions(): Unable to read directory '$_path': $!";
			return undef;
		}

		foreach my $parameter (readdir($pHandle)) {
			my $pPath = mkpath($sPath, $parameter);

			if ($parameter eq 'luns') {
				my $linked = readlink $pPath;
				my $g = SCST_GROUPS;
				my $l = SCST_LUNS;
				if ($linked =~ /\.\.\/\.\.\/$g\/(.*)\/$l/) {
					my $group = $1;
					$_sessions{$session}->{$parameter} =
					  $self->luns($driver, $target, $group);
				}
			} else {
				my $io = new IO::File $pPath, O_RDONLY;

				if (!$io) {
					$self->{'err_string'} = "sessions(): Unable to read ".
					  "session parameter '$parameter': $!";
					return undef;
				}

				my $value = <$io>;
				chomp $value;

				$_sessions{$session}->{parameter} = $value;
			}
		}
	}

	return \%_sessions;
}

sub sgvStats {
	my $self = shift;
	my %stats;

	my $sHandle = new IO::Handle;
	my $_path = mkpath(SCST_ROOT, SCST_SGV);
	if (!(opendir $sHandle, $_path)) {
		$self->{'err_string'} = "svgStats(): Unable to read directory '$_path': $!";
		return undef;
	}

	foreach my $stat (readdir($sHandle)) {
		next if (($stat eq '.') || ($stat eq '..'));

		my $sPath = mkpath(SCST_ROOT, SCST_SGV, $stat);

		if (-d $sPath) {
			my $lHandle = new IO::Handle;
			if (!(opendir $lHandle, $sPath)) {
				$self->{'err_string'} = "svgStats(): Unable to read directory '$sPath': $!";
				return undef;
			}

			foreach my $lief (readdir($lHandle)) {
				my $pPath = mkpath($sPath, $lief);

				my $io = new IO::File $pPath, O_RDONLY;

				if (!$io) {
					$self->{'err_string'} = "sgvStats(): Unable to read ".
					  "sgv stat '$stat/$lief': $!";
					return undef;
				}

				my $buffer;
				while (my $value = <$io>) {
					$buffer .= $value;
				}

				$stats{$stat}->{$lief} = $buffer;
			}
		} else {
			my $io = new IO::File $sPath, O_RDONLY;

			if (!$io) {
				$self->{'err_string'} = "sgvStats(): Unable to read ".
				  "sgv stat '$stat': $!";
				return undef;
			}

			my $buffer;
			while (my $value = <$io>) {
				$buffer .= $value;
			}

			$stats{$stat} = $buffer;
		}
	}

	return \%stats;
}

sub errorString {
	my $self = shift;

	return undef if (!$self->{'err_string'});

	my $string = $self->{'err_string'};
	$self->{'err_string'} = undef;

	return $string;
}

sub mkpath {
	my $path;

	foreach my $element (@_) {
		$path .= "/$element";
	}

	return $path;
}

;1 __END__

=head1 NAME

SCST::SCST - Generic SCST methods.

=head1 SYNOPSIS

    use SCST::SCST;

    $p = SCST::SCST->new();
    
    print "Using SCST version".$p->scstVersion()."\n";
    
    undef $p;

=head1 DESCRIPTION

Generic SCST methods.

=head2 Methods

=over 5

=item SCST::SCST->new();

Create a new SCST object. If the argument $debug is non-zero no changes
will be made.

Arguments: (bool) $debug

Returns: (object) $new

=item SCST::SCST->scstVersion();

Returns the version of SCST running.

Arguments: void

Returns: (string) $version

=back

=head1 WARNING

None at this time.

=head1 NOTES

None at this time.

=cut