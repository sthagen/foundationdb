/*
 * ChangeConfig.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/ClusterInterface.h"
#include "fdbserver/TesterInterface.actor.h"
#include "fdbclient/ManagementAPI.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbrpc/simulator.h"
#include "fdbclient/Schemas.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

struct ChangeConfigWorkload : TestWorkload {
	double minDelayBeforeChange, maxDelayBeforeChange;
	std::string configMode; //<\"single\"|\"double\"|\"triple\">
	std::string networkAddresses; //comma separated list e.g. "127.0.0.1:4000,127.0.0.1:4001"

	ChangeConfigWorkload(WorkloadContext const& wcx)
		: TestWorkload(wcx)
	{
		minDelayBeforeChange = getOption( options, LiteralStringRef("minDelayBeforeChange"), 0 );
		maxDelayBeforeChange = getOption( options, LiteralStringRef("maxDelayBeforeChange"), 0 );
		ASSERT( maxDelayBeforeChange >= minDelayBeforeChange );
		configMode = getOption( options, LiteralStringRef("configMode"), StringRef() ).toString();
		networkAddresses = getOption( options, LiteralStringRef("coordinators"), StringRef() ).toString();
	}

	std::string description() const override { return "ChangeConfig"; }

	Future<Void> start(Database const& cx) override {
		if( this->clientId != 0 ) return Void();
		return ChangeConfigClient( cx->clone(), this );
	}

	Future<bool> check(Database const& cx) override { return true; }

	void getMetrics(vector<PerfMetric>& m) override {}

	ACTOR Future<Void> extraDatabaseConfigure(ChangeConfigWorkload *self) {
		if (g_network->isSimulated() && g_simulator.extraDB) {
			auto extraFile = makeReference<ClusterConnectionFile>(*g_simulator.extraDB);
			state Database extraDB = Database::createDatabase(extraFile, -1);

			wait(delay(5*deterministicRandom()->random01()));
			if (self->configMode.size()) {
				wait(success(changeConfig(extraDB, self->configMode, true)));
				TraceEvent("WaitForReplicasExtra");
				wait( waitForFullReplication( extraDB ) );
				TraceEvent("WaitForReplicasExtraEnd");
			} if (self->networkAddresses.size()) {
				if (self->networkAddresses == "auto")
					wait(CoordinatorsChangeActor(extraDB, self, true));
				else
					wait(CoordinatorsChangeActor(extraDB, self));
			}
			wait(delay(5*deterministicRandom()->random01()));
		}
		return Void();
	}

	ACTOR Future<Void> ChangeConfigClient( Database cx, ChangeConfigWorkload *self) {
		wait( delay( self->minDelayBeforeChange + deterministicRandom()->random01() * ( self->maxDelayBeforeChange - self->minDelayBeforeChange ) ) );

		state bool extraConfigureBefore = deterministicRandom()->random01() < 0.5;

		if(extraConfigureBefore) {
			wait( self->extraDatabaseConfigure(self) );
		}

		if( self->configMode.size() ) {
			wait(success( changeConfig( cx, self->configMode, true ) ));
			TraceEvent("WaitForReplicas");
			wait( waitForFullReplication( cx ) );
			TraceEvent("WaitForReplicasEnd");
		}
		if( self->networkAddresses.size() ) {
			if (self->networkAddresses == "auto")
				wait(CoordinatorsChangeActor(cx, self, true));
			else
				wait(CoordinatorsChangeActor(cx, self));
		}

		if(!extraConfigureBefore) {
			wait( self->extraDatabaseConfigure(self) );
		}

		return Void();
	}

	ACTOR static Future<Void> CoordinatorsChangeActor(Database cx, ChangeConfigWorkload* self,
	                                                  bool autoChange = false) {
		state ReadYourWritesTransaction tr(cx);
		state int notEnoughMachineResults = 0; // Retry for the second time if we first get this result
		state std::string desiredCoordinatorsKey; // comma separated
		if (autoChange) { // if auto, we first get the desired addresses by read \xff\xff/management/auto_coordinators
			loop {
				try {
					Optional<Value> newCoordinatorsKey = wait(tr.get(
					    LiteralStringRef("auto_coordinators")
					        .withPrefix(SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::MANAGEMENT).begin)));
					ASSERT(newCoordinatorsKey.present());
					desiredCoordinatorsKey = newCoordinatorsKey.get().toString();
					tr.reset();
					break;
				} catch (Error& e) {
					if (e.code() == error_code_special_keys_api_failure) {
						Optional<Value> errorMsg =
						    wait(tr.get(SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::ERRORMSG).begin));
						ASSERT(errorMsg.present());
						std::string errorStr;
						auto valueObj = readJSONStrictly(errorMsg.get().toString()).get_obj();
						auto schema = readJSONStrictly(JSONSchemas::managementApiErrorSchema.toString()).get_obj();
						// special_key_space_management_api_error_msg schema validation
						TraceEvent(SevDebug, "GetAutoCoordinatorsChange")
						    .detail("ErrorMessage", valueObj["message"].get_str());
						ASSERT(schemaMatch(schema, valueObj, errorStr, SevError, true));
						ASSERT(valueObj["command"].get_str() == "auto_coordinators");
						if (valueObj["retriable"].get_bool() && notEnoughMachineResults < 1) {
							notEnoughMachineResults++;
							wait(delay(1.0));
							tr.reset();
						} else {
							break;
						}
					} else {
						wait(tr.onError(e));
					}
					wait(delay(FLOW_KNOBS->PREVENT_FAST_SPIN_DELAY));
				}
			}
		} else {
			desiredCoordinatorsKey = self->networkAddresses;
		}
		loop {
			try {
				tr.setOption(FDBTransactionOptions::SPECIAL_KEY_SPACE_ENABLE_WRITES);
				tr.set(LiteralStringRef("processes")
				           .withPrefix(SpecialKeySpace::getManagementApiCommandPrefix("coordinators")),
				       Value(desiredCoordinatorsKey));
				TraceEvent(SevDebug, "CoordinatorsChangeBeforeCommit")
				    .detail("Auto", autoChange)
				    .detail("NewCoordinatorsKey", describe(desiredCoordinatorsKey));
				wait(tr.commit());
				ASSERT(false);
			} catch (Error& e) {
				state Error err(e);
				if (e.code() == error_code_special_keys_api_failure) {
					Optional<Value> errorMsg =
					    wait(tr.get(SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::ERRORMSG).begin));
					ASSERT(errorMsg.present());
					std::string errorStr;
					auto valueObj = readJSONStrictly(errorMsg.get().toString()).get_obj();
					auto schema = readJSONStrictly(JSONSchemas::managementApiErrorSchema.toString()).get_obj();
					// special_key_space_management_api_error_msg schema validation
					TraceEvent(SevDebug, "CoordinatorsChangeError")
					    .detail("Auto", autoChange)
					    .detail("ErrorMessage", valueObj["message"].get_str());
					ASSERT(schemaMatch(schema, valueObj, errorStr, SevError, true));
					ASSERT(valueObj["command"].get_str() == "coordinators");
					break;
				} else {
					wait(tr.onError(err));
				}
				wait(delay(FLOW_KNOBS->PREVENT_FAST_SPIN_DELAY));
			}
		}
		return Void();
	}
};

WorkloadFactory<ChangeConfigWorkload> ChangeConfigWorkloadFactory("ChangeConfig");
