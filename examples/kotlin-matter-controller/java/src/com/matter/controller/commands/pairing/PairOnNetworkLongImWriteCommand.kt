/*
 *   Copyright (c) 2023 Project CHIP Authors
 *   All rights reserved.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *
 */
package com.matter.controller.commands.pairing

import com.matter.controller.commands.common.CredentialsIssuer
import java.util.logging.Level
import java.util.logging.Logger
import kotlinx.coroutines.runBlocking
import matter.controller.MatterController
import matter.controller.cluster.clusters.BasicInformationCluster

class PairOnNetworkLongImWriteCommand(
  controller: MatterController,
  credsIssue: CredentialsIssuer?
) :
  PairingCommand(
    controller,
    "onnetwork-long-im-write",
    credsIssue,
    PairingModeType.ON_NETWORK,
    PairingNetworkType.NONE,
    DiscoveryFilterType.LONG_DISCRIMINATOR
  ) {
  override fun runCommand() {
    currentCommissioner()
      .pairDevice(
        getNodeId(),
        getRemoteAddr().address.hostAddress,
        MATTER_PORT,
        getDiscriminator(),
        getSetupPINCode(),
      )
    currentCommissioner().setCompletionListener(this)
    waitCompleteMs(getTimeoutMillis())

    runBlocking {
      try {
        val basicInformationCluster =
          BasicInformationCluster(controller = currentCommissioner(), endpointId = DEFAULT_ENDPOINT)
        basicInformationCluster.writeNodeLabelAttribute("Test Node Label")
        logger.log(Level.INFO, "Write command succeeded")

        val nodeLabel = basicInformationCluster.readNodeLabelAttribute()
        logger.log(Level.INFO, "Read command succeeded, NodeLabel:${nodeLabel}")
      } catch (ex: Exception) {
        setFailure("invoke failure: ${ex.message}")
      } catch (ex: Exception) {
        logger.log(Level.WARNING, "General write failure occurred with error ${ex.message}")
        setFailure("invoke failure")
      } finally {
        clear()
      }
    }

    setSuccess()
  }

  companion object {
    private val logger = Logger.getLogger(PairOnNetworkLongImWriteCommand::class.java.name)
    private const val MATTER_PORT = 5540
    private const val DEFAULT_ENDPOINT: UShort = 0u
  }
}
