/**
 *  Copyright (C) 2024 FISCO BCOS.
 *  SPDX-License-Identifier: Apache-2.0
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 * @file TransactionResponse.h
 * @author: kyonGuo
 * @date 2024/4/16
 */

#pragma once
#include <bcos-framework/protocol/Block.h>
#include <bcos-framework/protocol/ProtocolTypeDef.h>
#include <bcos-framework/protocol/Transaction.h>
#include <bcos-utilities/Common.h>
#include <bcos-utilities/DataConvertUtility.h>
#include <json/json.h>

namespace bcos::rpc
{
// block and receipt are nullable
static void combineTxResponse(Json::Value& result, bcos::protocol::Transaction::ConstPtr&& tx,
    protocol::TransactionReceipt::ConstPtr&& receipt, bcos::protocol::Block::Ptr&& block)
{
    if (!result.isObject())
    {
        return;
    }
    size_t transactionIndex = 0;
    crypto::HashType blockHash;
    uint64_t blockNumber = 0;
    if (block)
    {
        blockHash = block->blockHeader()->hash();
        blockNumber = block->blockHeader()->number();
        for (; transactionIndex < block->transactionsHashSize(); transactionIndex++)
        {
            if (block->transactionHash(transactionIndex) == tx->hash())
            {
                break;
            }
        }
    }
    result["blockHash"] = blockHash.hexPrefixed();
    result["blockNumber"] = toQuantity(blockNumber);
    result["transactionIndex"] = toQuantity(transactionIndex);
    auto from = toHexStringWithPrefix(tx->sender());
    toChecksumAddress(from, bcos::crypto::keccak256Hash(from).hexPrefixed(), "0x");
    result["from"] = std::move(from);
    if (tx->to().empty())
    {
        result["to"] = Json::nullValue;
    }
    else
    {
        auto to = std::string(tx->to());
        toChecksumAddress(to, bcos::crypto::keccak256Hash(to).hex());
        result["to"] = "0x" + std::move(to);
    }
    result["gas"] = toQuantity(tx->gasLimit());
    result["gasPrice"] = std::string(receipt ? receipt->effectiveGasPrice() : tx->gasPrice());
    result["hash"] = tx->hash().hexPrefixed();
    result["input"] = toHexStringWithPrefix(tx->input());
    Web3Transaction web3Tx;
    auto extraBytesRef = bcos::bytesRef(
        const_cast<byte*>(tx->extraTransactionBytes().data()), tx->extraTransactionBytes().size());
    codec::rlp::decode(extraBytesRef, web3Tx);
    result["nonce"] = toQuantity(web3Tx.nonce);
    result["type"] = toQuantity(static_cast<uint8_t>(web3Tx.type));
    result["value"] = toQuantity(web3Tx.value);
    result["r"] = toQuantity(tx->signatureData().getCroppedData(0, 32));
    result["s"] = toQuantity(tx->signatureData().getCroppedData(32, 32));
    result["v"] = toQuantity(tx->signatureData().getCroppedData(64, 1));
    if (web3Tx.type >= TransactionType::EIP1559)
    {
        result["maxPriorityFeePerGas"] = toQuantity(web3Tx.maxPriorityFeePerGas);
        result["maxFeePerGas"] = toQuantity(web3Tx.maxFeePerGas);
    }
    result["chainId"] = toQuantity(web3Tx.chainId.value_or(0));
}
}  // namespace bcos::rpc
