#include "bcos-framework/ledger/LedgerConfig.h"
#include "bcos-framework/storage2/MemoryStorage.h"
#include "bcos-framework/storage2/Storage.h"
#include "bcos-framework/transaction-scheduler/TransactionScheduler.h"
#include "bcos-tars-protocol/protocol/BlockHeaderImpl.h"
#include "bcos-tars-protocol/protocol/TransactionReceiptFactoryImpl.h"
#include "bcos-task/Generator.h"
#include "bcos-transaction-scheduler/MultiLayerStorage.h"
#include <bcos-crypto/hash/Keccak256.h>
#include <bcos-framework/transaction-executor/TransactionExecutor.h>
#include <bcos-tars-protocol/protocol/TransactionImpl.h>
#include <bcos-task/Wait.h>
#include <bcos-transaction-scheduler/SchedulerParallelImpl.h>
#include <boost/test/unit_test.hpp>

using namespace bcos;
using namespace bcos::storage2;
using namespace bcos::transaction_executor;
using namespace bcos::transaction_scheduler;
using namespace std::string_view_literals;

struct MockExecutorParallel
{
    struct Context
    {
    };

    friend task::Task<Context> tag_invoke(
        transaction_executor::tag_t<createExecuteContext> /*unused*/,
        MockExecutorParallel& executor, auto& storage, protocol::BlockHeader const& blockHeader,
        protocol::Transaction const& transaction, int32_t contextID,
        ledger::LedgerConfig const& ledgerConfig)
    {
        co_return {};
    }

    template <int step>
    friend task::Task<protocol::TransactionReceipt::Ptr> tag_invoke(
        transaction_executor::tag_t<executeStep> /*unused*/, Context& executeContext)
    {
        co_return {};
    }

    friend task::Task<protocol::TransactionReceipt::Ptr> tag_invoke(
        bcos::transaction_executor::tag_t<
            bcos::transaction_executor::executeTransaction> /*unused*/,
        MockExecutorParallel& executor, auto& storage, protocol::BlockHeader const& blockHeader,
        protocol::Transaction const& transaction, int contextID, ledger::LedgerConfig const&,
        auto&& waitOperator, auto&&...)
    {
        co_return {};
    }
};

class TestSchedulerParallelFixture
{
public:
    using MutableStorage = memory_storage::MemoryStorage<StateKey, StateValue,
        memory_storage::Attribute(memory_storage::ORDERED | memory_storage::LOGICAL_DELETION)>;
    using BackendStorage = memory_storage::MemoryStorage<StateKey, StateValue,
        memory_storage::Attribute(memory_storage::ORDERED | memory_storage::CONCURRENT),
        std::hash<StateKey>>;

    TestSchedulerParallelFixture()
      : cryptoSuite(std::make_shared<bcos::crypto::CryptoSuite>(
            std::make_shared<bcos::crypto::Keccak256>(), nullptr, nullptr)),
        receiptFactory(cryptoSuite),
        multiLayerStorage(backendStorage)
    {}

    BackendStorage backendStorage;
    bcos::crypto::CryptoSuite::Ptr cryptoSuite;
    bcostars::protocol::TransactionReceiptFactoryImpl receiptFactory;
    MultiLayerStorage<MutableStorage, void, BackendStorage> multiLayerStorage;
    crypto::Hash::Ptr hashImpl = std::make_shared<bcos::crypto::Keccak256>();
};

BOOST_FIXTURE_TEST_SUITE(TestSchedulerParallel, TestSchedulerParallelFixture)

BOOST_AUTO_TEST_CASE(simple)
{
    task::syncWait([&, this]() -> task::Task<void> {
        MockExecutorParallel executor;
        SchedulerParallelImpl<MutableStorage> scheduler;

        bcostars::protocol::BlockHeaderImpl blockHeader(
            [inner = bcostars::BlockHeader()]() mutable { return std::addressof(inner); });
        auto transactions =
            RANGES::iota_view<int, int>(0, 100) | RANGES::views::transform([](int index) {
                return std::make_unique<bcostars::protocol::TransactionImpl>(
                    [inner = bcostars::Transaction()]() mutable { return std::addressof(inner); });
            }) |
            RANGES::to<std::vector<std::unique_ptr<bcostars::protocol::TransactionImpl>>>();

        auto view = fork(multiLayerStorage);
        newMutable(view);
        ledger::LedgerConfig ledgerConfig;
        auto receipts = co_await bcos::transaction_scheduler::executeBlock(scheduler, view,
            executor, blockHeader,
            transactions | RANGES::views::transform([](auto& ptr) -> auto& { return *ptr; }),
            ledgerConfig);
        BOOST_CHECK_EQUAL(transactions.size(), receipts.size());

        co_return;
    }());
    // Wait for tbb
}

constexpr static size_t MOCK_USER_COUNT = 1000;
struct MockConflictExecutor
{
    template <class Storage>
    struct Context
    {
        protocol::Transaction const* transaction;
        Storage* storage;

        std::string fromAddress;
        std::string toAddress;
    };

    friend auto tag_invoke(transaction_executor::tag_t<createExecuteContext> /*unused*/,
        MockConflictExecutor& executor, auto& storage, protocol::BlockHeader const& blockHeader,
        protocol::Transaction const& transaction, int32_t contextID,
        ledger::LedgerConfig const& ledgerConfig)
        -> task::Task<Context<std::decay_t<decltype(storage)>>>
    {
        co_return {.transaction = std::addressof(transaction),
            .storage = std::addressof(storage),
            .fromAddress = {},
            .toAddress = {}};
    }

    template <int step>
    friend task::Task<protocol::TransactionReceipt::Ptr> tag_invoke(
        transaction_executor::tag_t<executeStep> /*unused*/, auto& executeContext)
    {
        if constexpr (step == 0)
        {
            auto input = executeContext.transaction->input();
            auto inputNum =
                boost::lexical_cast<int>(std::string_view((const char*)input.data(), input.size()));
            executeContext.fromAddress = std::to_string(inputNum % MOCK_USER_COUNT);
            executeContext.toAddress =
                std::to_string((inputNum + (MOCK_USER_COUNT / 2)) % MOCK_USER_COUNT);
        }
        else if constexpr (step == 1)
        {
            StateKey fromKey{"t_test"sv, executeContext.fromAddress};
            auto fromEntry = co_await storage2::readOne(*executeContext.storage, fromKey);
            fromEntry->set(
                boost::lexical_cast<std::string>(boost::lexical_cast<int>(fromEntry->get()) - 1));
            co_await storage2::writeOne(*executeContext.storage, fromKey, *fromEntry);

            // Read toKey and +1
            StateKey toKey{"t_test"sv, executeContext.toAddress};
            auto toEntry = co_await storage2::readOne(*executeContext.storage, toKey);
            toEntry->set(
                boost::lexical_cast<std::string>(boost::lexical_cast<int>(toEntry->get()) + 1));
            co_await storage2::writeOne(*executeContext.storage, toKey, *toEntry);
        }
        else if constexpr (step == 2)
        {
            co_return std::shared_ptr<bcos::protocol::TransactionReceipt>(
                (bcos::protocol::TransactionReceipt*)0x10086, [](auto* p) {});
        }
        co_return {};
    }
};

BOOST_AUTO_TEST_CASE(conflict)
{
    task::syncWait([&, this]() -> task::Task<void> {
        MockConflictExecutor executor;
        SchedulerParallelImpl<MutableStorage> scheduler;

        auto view1 = fork(multiLayerStorage);
        newMutable(view1);
        pushView(multiLayerStorage, std::move(view1));

        constexpr static int INITIAL_VALUE = 100000;
        for (auto i : RANGES::views::iota(0LU, MOCK_USER_COUNT))
        {
            StateKey key{"t_test"sv, boost::lexical_cast<std::string>(i)};
            storage::Entry entry;
            entry.set(boost::lexical_cast<std::string>(INITIAL_VALUE));
            co_await storage2::writeOne(*frontStorage(multiLayerStorage), key, std::move(entry));
        }

        bcostars::protocol::BlockHeaderImpl blockHeader(
            [inner = bcostars::BlockHeader()]() mutable { return std::addressof(inner); });
        constexpr static auto TRANSACTION_COUNT = 1000;
        auto transactions =
            RANGES::views::iota(0, TRANSACTION_COUNT) | RANGES::views::transform([](int index) {
                auto transaction = std::make_unique<bcostars::protocol::TransactionImpl>();
                auto num = boost::lexical_cast<std::string>(index);
                transaction->mutableInner().data.input.assign(num.begin(), num.end());

                return transaction;
            }) |
            RANGES::to<std::vector<std::unique_ptr<bcostars::protocol::TransactionImpl>>>();

        auto transactionRefs =
            transactions | RANGES::views::transform([](auto& ptr) -> auto& { return *ptr; });
        auto view = fork(multiLayerStorage);
        newMutable(view);
        ledger::LedgerConfig ledgerConfig;
        auto receipts = co_await bcos::transaction_scheduler::executeBlock(
            scheduler, view, executor, blockHeader, transactionRefs, ledgerConfig);
        pushView(multiLayerStorage, std::move(view));

        for (auto i : RANGES::views::iota(0LU, MOCK_USER_COUNT))
        {
            StateKey key{"t_test"sv, boost::lexical_cast<std::string>(i)};
            auto entry = co_await storage2::readOne(*frontStorage(multiLayerStorage), key);
            BOOST_CHECK_EQUAL(boost::lexical_cast<int>(entry->get()), INITIAL_VALUE);
        }
        for (auto const& receipt : receipts)
        {
            if (!receipt)
            {
                BOOST_FAIL("receipt is null!");
            }
            BOOST_CHECK_EQUAL(receipt.get(), (bcos::protocol::TransactionReceipt*)0x10086);
        }

        co_return;
    }());
}

BOOST_AUTO_TEST_SUITE_END()