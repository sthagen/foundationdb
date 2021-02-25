#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__INTEL_COMPILER)
#define BOOST_SYSTEM_NO_LIB
#define BOOST_DATE_TIME_NO_LIB
#define BOOST_REGEX_NO_LIB
#include <boost/process.hpp>
#endif
#include "fdbserver/FDBExecHelper.actor.h"
#include "flow/Trace.h"
#include "flow/flow.h"
#include "fdbclient/versions.h"
#include "fdbserver/Knobs.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

ExecCmdValueString::ExecCmdValueString(StringRef pCmdValueString) {
	cmdValueString = pCmdValueString;
	parseCmdValue();
}

void ExecCmdValueString::setCmdValueString(StringRef pCmdValueString) {
	// reset everything
	binaryPath = StringRef();

	// set the new cmdValueString
	cmdValueString = pCmdValueString;

	// parse it out
	parseCmdValue();
}

StringRef ExecCmdValueString::getCmdValueString() const {
	return cmdValueString.toString();
}

StringRef ExecCmdValueString::getBinaryPath() const {
	return binaryPath;
}

VectorRef<StringRef> ExecCmdValueString::getBinaryArgs() const {
	return binaryArgs;
}

void ExecCmdValueString::parseCmdValue() {
	StringRef param = this->cmdValueString;
	// get the binary path
	this->binaryPath = param.eat(LiteralStringRef(" "));

	// no arguments provided
	if (param == StringRef()) {
		return;
	}

	// extract the arguments
	while (param != StringRef()) {
		StringRef token = param.eat(LiteralStringRef(" "));
		this->binaryArgs.push_back(this->binaryArgs.arena(), token);
	}
	return;
}

void ExecCmdValueString::dbgPrint() const {
	auto te = TraceEvent("ExecCmdValueString");

	te.detail("CmdValueString", cmdValueString.toString());
	te.detail("BinaryPath", binaryPath.toString());

	int i = 0;
	for (auto elem : binaryArgs) {
		te.detail(format("Arg", ++i).c_str(), elem.toString());
	}
	return;
}

#if defined(_WIN32) || defined(__APPLE__) || defined(__INTEL_COMPILER)
ACTOR Future<int> spawnProcess(std::string binPath, std::vector<std::string> paramList, double maxWaitTime, bool isSync, double maxSimDelayTime)
{
	wait(delay(0.0));
	return 0;
}
#else

static auto fork_child(const std::string& path, std::vector<char*>& paramList) {
	int pipefd[2];
	pipe(pipefd);
	auto readFD = pipefd[0];
	auto writeFD = pipefd[1];
	pid_t pid = fork();
	if (pid == -1) {
		close(readFD);
		close(writeFD);
		return std::make_pair(-1, Optional<int>{});
	}
	if (pid == 0) {
		close(readFD);
		dup2(writeFD, 1); // stdout
		dup2(writeFD, 2); // stderr
		close(writeFD);
		execv(&path[0], &paramList[0]);
		_exit(EXIT_FAILURE);
	}
	close(writeFD);
	return std::make_pair(pid, Optional<int>{ readFD });
}

static void setupTraceWithOutput(TraceEvent& event, size_t bytesRead, char* outputBuffer) {
	if (bytesRead == 0) return;
	ASSERT(bytesRead <= SERVER_KNOBS->MAX_FORKED_PROCESS_OUTPUT);
	auto extraBytesNeeded = std::max<int>(bytesRead - event.getMaxFieldLength(), 0);
	event.setMaxFieldLength(event.getMaxFieldLength() + extraBytesNeeded);
	event.setMaxEventLength(event.getMaxEventLength() + extraBytesNeeded);
	outputBuffer[bytesRead - 1] = '\0';
	event.detail("Output", std::string(outputBuffer));
}

ACTOR Future<int> spawnProcess(std::string path, std::vector<std::string> args, double maxWaitTime, bool isSync, double maxSimDelayTime)
{
	// for async calls in simulator, always delay by a deterministic amount of time and then
	// do the call synchronously, otherwise the predictability of the simulator breaks
	if (!isSync && g_network->isSimulated()) {
		double snapDelay = std::max(maxSimDelayTime - 1, 0.0);
		// add some randomness
		snapDelay += deterministicRandom()->random01();
		TraceEvent("SnapDelaySpawnProcess")
				.detail("SnapDelay", snapDelay);
		wait(delay(snapDelay));
	}

	std::vector<char*> paramList;
	for (int i = 0; i < args.size(); i++) {
		paramList.push_back(&args[i][0]);
	}
	paramList.push_back(nullptr);

	state std::string allArgs;
	for (int i = 0; i < args.size(); i++) {
		if (i > 0) allArgs += " ";
		allArgs += args[i];
	}

	state std::pair<pid_t, Optional<int>> pidAndReadFD = fork_child(path, paramList);
	state pid_t pid = pidAndReadFD.first;
	state Optional<int> readFD = pidAndReadFD.second;
	if (pid == -1) {
		TraceEvent(SevWarnAlways, "SpawnProcess: Command failed to spawn")
			.detail("Cmd", path)
			.detail("Args", allArgs);
		return -1;
	} else if (pid > 0) {
		state int status = -1;
		state double runTime = 0;
		state Arena arena;
		state char* outputBuffer = new (arena) char[SERVER_KNOBS->MAX_FORKED_PROCESS_OUTPUT];
		state size_t bytesRead = 0;
		while (true) {
			if (runTime > maxWaitTime) {
				// timing out

				TraceEvent(SevWarnAlways, "SpawnProcess : Command failed, timeout")
					.detail("Cmd", path)
					.detail("Args", allArgs);
				return -1;
			}
			int err = waitpid(pid, &status, WNOHANG);
			loop {
				int bytes =
				    read(readFD.get(), &outputBuffer[bytesRead], SERVER_KNOBS->MAX_FORKED_PROCESS_OUTPUT - bytesRead);
				bytesRead += bytes;
				if (bytes == 0) break;
			}

			if (err < 0) {
				TraceEvent event(SevWarnAlways, "SpawnProcess : Command failed");
				setupTraceWithOutput(event, bytesRead, outputBuffer);
				event.detail("Cmd", path)
				    .detail("Args", allArgs)
				    .detail("Errno", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
				return -1;
			} else if (err == 0) {
				// child process has not completed yet
				if (isSync || g_network->isSimulated()) {
					// synchronously sleep
					threadSleep(0.1);
				} else {
					// yield for other actors to run
					wait(delay(0.1));
				}
				runTime += 0.1;
			} else {
				// child process completed
				if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
					TraceEvent event(SevWarnAlways, "SpawnProcess : Command failed");
					setupTraceWithOutput(event, bytesRead, outputBuffer);
					event.detail("Cmd", path)
					    .detail("Args", allArgs)
					    .detail("Errno", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
					return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
				}
				TraceEvent event("SpawnProcess : Command status");
				setupTraceWithOutput(event, bytesRead, outputBuffer);
				event.detail("Cmd", path)
				    .detail("Args", allArgs)
				    .detail("Errno", WIFEXITED(status) ? WEXITSTATUS(status) : 0);
				return 0;
			}
		}
	}
	return -1;

}
#endif

ACTOR Future<int> execHelper(ExecCmdValueString* execArg, UID snapUID, std::string folder, std::string role) {
	state Standalone<StringRef> uidStr(snapUID.toString());
	state int err = 0;
	state Future<int> cmdErr;
	state double maxWaitTime = SERVER_KNOBS->SNAP_CREATE_MAX_TIMEOUT;
	if (!g_network->isSimulated()) {
		// get bin path
		auto snapBin = execArg->getBinaryPath();
		std::vector<std::string> paramList;
		paramList.push_back(snapBin.toString());
		// get user passed arguments
		auto listArgs = execArg->getBinaryArgs();
		for (auto elem : listArgs) {
			paramList.push_back(elem.toString());
		}
		// get additional arguments
		paramList.push_back("--path");
		paramList.push_back(folder);
		const char* version = FDB_VT_VERSION;
		paramList.push_back("--version");
		paramList.push_back(version);
		paramList.push_back("--role");
		paramList.push_back(role);
		paramList.push_back("--uid");
		paramList.push_back(uidStr.toString());
		cmdErr = spawnProcess(snapBin.toString(), paramList, maxWaitTime, false /*isSync*/, 0);
		wait(success(cmdErr));
		err = cmdErr.get();
	} else {
		// copy the files
		state std::string folderFrom = folder + "/.";
		state std::string folderTo = folder + "-snap-" + uidStr.toString();
		double maxSimDelayTime = 10.0;
		folderTo = folder + "-snap-" + uidStr.toString() + "-" + role;
		std::vector<std::string> paramList;
		std::string mkdirBin = "/bin/mkdir";
		paramList.push_back(mkdirBin);
		paramList.push_back(folderTo);
		cmdErr = spawnProcess(mkdirBin, paramList, maxWaitTime, false /*isSync*/, maxSimDelayTime);
		wait(success(cmdErr));
		err = cmdErr.get();
		if (err == 0) {
			std::vector<std::string> paramList;
			std::string cpBin = "/bin/cp";
			paramList.push_back(cpBin);
			paramList.push_back("-a");
			paramList.push_back(folderFrom);
			paramList.push_back(folderTo);
			cmdErr = spawnProcess(cpBin, paramList, maxWaitTime, true /*isSync*/, 1.0);
			wait(success(cmdErr));
			err = cmdErr.get();
		}
	}
	return err;
}

struct StorageVersionInfo {
	Version version;
	Version durableVersion;
};

// storage nodes get snapshotted through the worker interface which does not have context about version information,
// following info is gathered at worker level to facilitate printing of version info during storage snapshots.
typedef std::map<UID, StorageVersionInfo> UidStorageVersionInfo;

std::map<NetworkAddress, UidStorageVersionInfo> workerStorageVersionInfo;

void setDataVersion(UID uid, Version version) {
	NetworkAddress addr = g_network->getLocalAddress();
	workerStorageVersionInfo[addr][uid].version = version;
}

void setDataDurableVersion(UID uid, Version durableVersion) {
	NetworkAddress addr = g_network->getLocalAddress();
	workerStorageVersionInfo[addr][uid].durableVersion = durableVersion;
}

void printStorageVersionInfo() {
	NetworkAddress addr = g_network->getLocalAddress();
	for (auto itr = workerStorageVersionInfo[addr].begin(); itr != workerStorageVersionInfo[addr].end(); itr++) {
		TraceEvent("StorageVersionInfo")
			.detail("UID", itr->first)
			.detail("Version", itr->second.version)
			.detail("DurableVersion", itr->second.durableVersion);
	}
}
