#include <Disks/ObjectStorages/DiskObjectStorageTransaction.h>
#include <Disks/ObjectStorages/DiskObjectStorage.h>
#if ENABLE_DISTRIBUTED_CACHE
#include <Disks/IO/WriteBufferFromDistributedCache.h>
#include <Core/DistributedCacheProtocol.h>
#endif
#include <Core/Settings.h>
#include <Disks/IO/WriteBufferWithFinalizeCallback.h>
#include <Interpreters/Context.h>
#include <Common/checkStackSize.h>
#include <ranges>
#include <Common/logger_useful.h>
#include <Common/Exception.h>
#include <Disks/WriteMode.h>
#include <base/defines.h>
#include <Common/FailPoint.h>
#include <Disks/IDisk.h>
#include <Disks/ObjectStorages/MetadataStorageFromDisk.h>
#include <Disks/ObjectStorages/MetadataStorageFromPlainObjectStorageOperations.h>
#include <boost/algorithm/string/join.hpp>

#include <fmt/ranges.h>

namespace DB
{

namespace FailPoints
{
    extern const char smt_insert_fake_hardware_error[];
}

namespace ErrorCodes
{
    extern const int UNKNOWN_FORMAT;
    extern const int ATTEMPT_TO_READ_AFTER_EOF;
    extern const int CANNOT_READ_ALL_DATA;
    extern const int CANNOT_OPEN_FILE;
    extern const int FILE_DOESNT_EXIST;
    extern const int CANNOT_PARSE_INPUT_ASSERTION_FAILED;
}

DiskObjectStorageTransaction::DiskObjectStorageTransaction(
    IObjectStorage & object_storage_,
    IMetadataStorage & metadata_storage_)
    : object_storage(object_storage_)
    , metadata_storage(metadata_storage_)
    , metadata_transaction(metadata_storage.createTransaction())
{}


DiskObjectStorageTransaction::DiskObjectStorageTransaction(
    IObjectStorage & object_storage_,
    IMetadataStorage & metadata_storage_,
    MetadataTransactionPtr metadata_transaction_)
    : object_storage(object_storage_)
    , metadata_storage(metadata_storage_)
    , metadata_transaction(metadata_transaction_)
{}

MultipleDisksObjectStorageTransaction::MultipleDisksObjectStorageTransaction(
    IObjectStorage & object_storage_,
    IMetadataStorage & metadata_storage_,
    IObjectStorage & destination_object_storage_,
    IMetadataStorage & destination_metadata_storage_)
    : DiskObjectStorageTransaction(object_storage_, metadata_storage_, destination_metadata_storage_.createTransaction())
    , destination_object_storage(destination_object_storage_)
    , destination_metadata_storage(destination_metadata_storage_)
{}

namespace
{
/// Operation which affects only metadata. Simplest way to
/// implement via callback.
struct PureMetadataObjectStorageOperation final : public IDiskObjectStorageOperation
{
    std::function<void(MetadataTransactionPtr tx)> on_execute;

    PureMetadataObjectStorageOperation(
        IObjectStorage & object_storage_,
        IMetadataStorage & metadata_storage_,
        std::function<void(MetadataTransactionPtr tx)> && on_execute_)
        : IDiskObjectStorageOperation(object_storage_, metadata_storage_)
        , on_execute(std::move(on_execute_))
    {}

    void execute(MetadataTransactionPtr transaction) override
    {
        on_execute(transaction);
    }

    void undo() override
    {
    }

    void finalize() override
    {
    }

    std::string getInfoForLog() const override { return fmt::format("PureMetadataObjectStorageOperation"); }
};


struct ObjectsToRemove
{
    StoredObjects objects;
    UnlinkMetadataFileOperationOutcomePtr unlink_outcome;
};

struct RemoveObjectStorageOperation final : public IDiskObjectStorageOperation
{
    std::string path;
    bool delete_metadata_only;
    ObjectsToRemove objects_to_remove;
    bool if_exists;
    bool remove_from_cache = false;

    RemoveObjectStorageOperation(
        IObjectStorage & object_storage_,
        IMetadataStorage & metadata_storage_,
        const std::string & path_,
        bool delete_metadata_only_,
        bool if_exists_)
        : IDiskObjectStorageOperation(object_storage_, metadata_storage_)
        , path(path_)
        , delete_metadata_only(delete_metadata_only_)
        , if_exists(if_exists_)
    {}

    std::string getInfoForLog() const override
    {
        return fmt::format("RemoveObjectStorageOperation (path: {}, if exists: {})", path, if_exists);
    }

    void execute(MetadataTransactionPtr tx) override
    {
        if (!metadata_storage.existsFile(path))
        {
            auto maybe_blobs = tx->tryGetBlobsFromTransactionIfExists(path);
            if (maybe_blobs.has_value())
            {
                auto unlink_outcome = tx->unlinkMetadata(path);
                if (unlink_outcome)
                    objects_to_remove = ObjectsToRemove{std::move(*maybe_blobs), std::move(unlink_outcome)};

                return;
            }

            if (if_exists)
                return;

            throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "Metadata path '{}' doesn't exist or isn't a regular file", path);
        }

        try
        {
            auto objects = metadata_storage.getStorageObjects(path);

            auto unlink_outcome = tx->unlinkMetadata(path);

            if (unlink_outcome)
                objects_to_remove = ObjectsToRemove{std::move(objects), std::move(unlink_outcome)};
        }
        catch (const Exception & e)
        {
            /// If it's impossible to read meta - just remove it from FS.
            if (e.code() == ErrorCodes::UNKNOWN_FORMAT
                || e.code() == ErrorCodes::ATTEMPT_TO_READ_AFTER_EOF
                || e.code() == ErrorCodes::CANNOT_READ_ALL_DATA
                || e.code() == ErrorCodes::CANNOT_OPEN_FILE)
            {
                tx->unlinkFile(path);
            }
            else
                throw;
        }
    }

    void undo() override
    {

    }

    void finalize() override
    {
        /// The client for an object storage may do retries internally
        /// and there could be a situation when a query succeeded, but the response is lost
        /// due to network error or similar. And when it will retry an operation it may receive
        /// a 404 HTTP code. We don't want to threat this code as a real error for deletion process
        /// (e.g. throwing some exceptions) and thus we just use method `removeObjectsIfExists`
        if (!delete_metadata_only && !objects_to_remove.objects.empty()
            && objects_to_remove.unlink_outcome->num_hardlinks == 0)
        {
            object_storage.removeObjectsIfExist(objects_to_remove.objects);
        }
    }
};

struct RemoveManyObjectStorageOperation final : public IDiskObjectStorageOperation
{
    const RemoveBatchRequest remove_paths;
    const bool keep_all_batch_data;
    const NameSet file_names_remove_metadata_only;

    std::vector<String> paths_removed_with_objects;
    std::vector<ObjectsToRemove> objects_to_remove;

    RemoveManyObjectStorageOperation(
        IObjectStorage & object_storage_,
        IMetadataStorage & metadata_storage_,
        const RemoveBatchRequest & remove_paths_,
        bool keep_all_batch_data_,
        const NameSet & file_names_remove_metadata_only_)
        : IDiskObjectStorageOperation(object_storage_, metadata_storage_)
        , remove_paths(remove_paths_)
        , keep_all_batch_data(keep_all_batch_data_)
        , file_names_remove_metadata_only(file_names_remove_metadata_only_)
    {}

    std::string getInfoForLog() const override
    {
        return fmt::format("RemoveManyObjectStorageOperation (paths size: {}, keep all batch {}, files to keep {})", remove_paths.size(), keep_all_batch_data, fmt::join(file_names_remove_metadata_only, ", "));
    }

    void execute(MetadataTransactionPtr tx) override
    {
        for (const auto & [path, if_exists] : remove_paths)
        {
            if (!metadata_storage.existsFile(path))
            {
                auto maybe_blobs = tx->tryGetBlobsFromTransactionIfExists(path);
                if (maybe_blobs.has_value())
                {
                    auto unlink_outcome = tx->unlinkMetadata(path);
                    if (unlink_outcome && !keep_all_batch_data && !file_names_remove_metadata_only.contains(fs::path(path).filename()))
                    {
                        objects_to_remove.emplace_back(ObjectsToRemove{std::move(*maybe_blobs), std::move(unlink_outcome)});
                        paths_removed_with_objects.push_back(path);
                    }

                    continue;
                }

                if (if_exists)
                    continue;

                throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "Metadata path '{}' doesn't exist or isn't a regular file", path);
            }

            try
            {
                auto objects = metadata_storage.getStorageObjects(path);
                auto unlink_outcome = tx->unlinkMetadata(path);
                if (unlink_outcome && !keep_all_batch_data && !file_names_remove_metadata_only.contains(fs::path(path).filename()))
                {
                    objects_to_remove.emplace_back(ObjectsToRemove{std::move(objects), std::move(unlink_outcome)});
                    paths_removed_with_objects.push_back(path);
                }
            }
            catch (const Exception & e)
            {
                /// If it's impossible to read meta - just remove it from FS.
                if (e.code() == ErrorCodes::UNKNOWN_FORMAT
                    || e.code() == ErrorCodes::ATTEMPT_TO_READ_AFTER_EOF
                    || e.code() == ErrorCodes::CANNOT_READ_ALL_DATA
                    || e.code() == ErrorCodes::CANNOT_OPEN_FILE)
                {
                    LOG_DEBUG(
                        getLogger("RemoveManyObjectStorageOperation"),
                        "Can't read metadata because of an exception. Just remove it from the filesystem. Path: {}, exception: {}",
                        metadata_storage.getPath() + path,
                        e.message());

                    tx->unlinkFile(path);
                }
                else
                    throw;
            }
        }
    }

    void undo() override
    {
    }

    void finalize() override
    {
        StoredObjects remove_from_remote;
        for (auto && [objects, unlink_outcome] : objects_to_remove)
        {
            if (unlink_outcome->num_hardlinks == 0)
                std::move(objects.begin(), objects.end(), std::back_inserter(remove_from_remote));
        }

        /// Read comment inside RemoveObjectStorageOperation class
        /// TL;DR Don't pay any attention to 404 status code
        if (!remove_from_remote.empty())
            object_storage.removeObjectsIfExist(remove_from_remote);

        if (!keep_all_batch_data)
        {
            LOG_DEBUG(
                getLogger("RemoveManyObjectStorageOperation"),
                "metadata and objects were removed for [{}], "
                "only metadata were removed for [{}].",
                boost::algorithm::join(paths_removed_with_objects, ", "),
                boost::algorithm::join(file_names_remove_metadata_only, ", "));
        }
    }
};


struct RemoveRecursiveObjectStorageOperation final : public IDiskObjectStorageOperation
{
    /// path inside disk with metadata
    const std::string path;
    const bool keep_all_batch_data;
    /// paths inside the 'this->path'
    const NameSet file_names_remove_metadata_only;

    /// map from local_path to its remote objects with hardlinks counter
    /// local_path is the path inside 'this->path'
    std::unordered_map<std::string, ObjectsToRemove> objects_to_remove_by_path;

    RemoveRecursiveObjectStorageOperation(
        IObjectStorage & object_storage_,
        IMetadataStorage & metadata_storage_,
        const std::string & path_,
        bool keep_all_batch_data_,
        const NameSet & file_names_remove_metadata_only_)
        : IDiskObjectStorageOperation(object_storage_, metadata_storage_)
        , path(path_)
        , keep_all_batch_data(keep_all_batch_data_)
        , file_names_remove_metadata_only(file_names_remove_metadata_only_)
    {}

    std::string getInfoForLog() const override
    {
        return fmt::format("RemoveRecursiveObjectStorageOperation (path: {})", path);
    }

    void removeMetadataRecursive(MetadataTransactionPtr tx, const std::string & path_to_remove)
    {
        checkStackSize(); /// This is needed to prevent stack overflow in case of cyclic symlinks.

        if (metadata_storage.existsFile(path_to_remove))
        {
            try
            {
                chassert(path_to_remove.starts_with(path));
                auto rel_path = String(fs::relative(fs::path(path_to_remove), fs::path(path)));

                auto objects_paths = metadata_storage.getStorageObjects(path_to_remove);
                auto unlink_outcome = tx->unlinkMetadata(path_to_remove);

                if (unlink_outcome && !file_names_remove_metadata_only.contains(rel_path))
                {
                    objects_to_remove_by_path[std::move(rel_path)]
                        = ObjectsToRemove{std::move(objects_paths), std::move(unlink_outcome)};
                }
            }
            catch (const Exception & e)
            {
                /// If it's impossible to read meta - just remove it from FS.
                if (e.code() == ErrorCodes::UNKNOWN_FORMAT
                    || e.code() == ErrorCodes::ATTEMPT_TO_READ_AFTER_EOF
                    || e.code() == ErrorCodes::CANNOT_READ_ALL_DATA
                    || e.code() == ErrorCodes::CANNOT_OPEN_FILE
                    || e.code() == ErrorCodes::CANNOT_PARSE_INPUT_ASSERTION_FAILED)
                {
                    LOG_DEBUG(
                        getLogger("RemoveRecursiveObjectStorageOperation"),
                        "Can't read metadata because of an exception. Just remove it from the filesystem. Path: {}, exception: {}",
                        metadata_storage.getPath() + path_to_remove,
                        e.message());

                    tx->unlinkFile(path_to_remove);
                }
                else
                    throw;
            }
        }
        else
        {
            for (auto it = metadata_storage.iterateDirectory(path_to_remove); it->isValid(); it->next())
                removeMetadataRecursive(tx, it->path());

            tx->removeDirectory(path_to_remove);
        }
    }

    void execute(MetadataTransactionPtr tx) override
    {
        /// Similar to DiskLocal and https://en.cppreference.com/w/cpp/filesystem/remove
        if (metadata_storage.existsFileOrDirectory(path))
            removeMetadataRecursive(tx, path);
    }

    void undo() override
    {
    }

    void finalize() override
    {
        if (!keep_all_batch_data)
        {
            std::vector<String> total_removed_paths;
            total_removed_paths.reserve(objects_to_remove_by_path.size());

            StoredObjects remove_from_remote;
            for (auto && [local_path, objects_to_remove] : objects_to_remove_by_path)
            {
                chassert(!file_names_remove_metadata_only.contains(local_path));
                if (objects_to_remove.unlink_outcome->num_hardlinks == 0)
                {
                    std::move(objects_to_remove.objects.begin(), objects_to_remove.objects.end(), std::back_inserter(remove_from_remote));
                    total_removed_paths.push_back(local_path);
                }
            }

            /// Read comment inside RemoveObjectStorageOperation class
            /// TL;DR Don't pay any attention to 404 status code
            object_storage.removeObjectsIfExist(remove_from_remote);

            LOG_DEBUG(
                getLogger("RemoveRecursiveObjectStorageOperation"),
                "Recursively remove path {}: "
                "metadata and objects were removed for [{}], "
                "only metadata were removed for [{}].",
                path,
                boost::algorithm::join(total_removed_paths, ", "),
                boost::algorithm::join(file_names_remove_metadata_only, ", "));
        }
    }
};


struct ReplaceFileObjectStorageOperation final : public IDiskObjectStorageOperation
{
    std::string path_from;
    std::string path_to;
    StoredObjects objects_to_remove;

    ReplaceFileObjectStorageOperation(
        IObjectStorage & object_storage_,
        IMetadataStorage & metadata_storage_,
        const std::string & path_from_,
        const std::string & path_to_)
        : IDiskObjectStorageOperation(object_storage_, metadata_storage_)
        , path_from(path_from_)
        , path_to(path_to_)
    {}

    std::string getInfoForLog() const override
    {
        return fmt::format("ReplaceFileObjectStorageOperation (path_from: {}, path_to: {})", path_from, path_to);
    }

    void execute(MetadataTransactionPtr tx) override
    {
        if (metadata_storage.existsFile(path_to))
        {
            if (metadata_storage.getType() != MetadataStorageType::PlainRewritable
                && metadata_storage.getType() != MetadataStorageType::Plain)
            {
                //  MetadataStorageFromPlainObjectStorageTransaction removes the source objects by itself.
                objects_to_remove = metadata_storage.getStorageObjects(path_to);
            }
            tx->replaceFile(path_from, path_to);
        }
        else
            tx->moveFile(path_from, path_to);
    }

    void undo() override
    {

    }

    void finalize() override
    {
        /// Read comment inside RemoveObjectStorageOperation class
        /// TL;DR Don't pay any attention to 404 status code
        if (!objects_to_remove.empty())
            object_storage.removeObjectsIfExist(objects_to_remove);
    }
};

struct WriteFileObjectStorageOperation final : public IDiskObjectStorageOperation
{
    StoredObject object;
    std::function<void(MetadataTransactionPtr)> on_execute;

    WriteFileObjectStorageOperation(
        IObjectStorage & object_storage_,
        IMetadataStorage & metadata_storage_,
        const StoredObject & object_)
        : IDiskObjectStorageOperation(object_storage_, metadata_storage_)
        , object(object_)
    {}

    std::string getInfoForLog() const override
    {
        return fmt::format("WriteFileObjectStorageOperation");
    }

    void setOnExecute(std::function<void(MetadataTransactionPtr)> && on_execute_)
    {
        on_execute = on_execute_;
    }

    void execute(MetadataTransactionPtr tx) override
    {
        if (on_execute)
            on_execute(tx);
    }

    void undo() override
    {
        object_storage.removeObjectIfExists(object);
    }

    void finalize() override
    {
    }
};


struct CopyFileObjectStorageOperation final : public IDiskObjectStorageOperation
{
    ReadSettings read_settings;
    WriteSettings write_settings;

    /// Local paths
    std::string from_path;
    std::string to_path;

    StoredObjects created_objects;
    IObjectStorage & destination_object_storage;

    CopyFileObjectStorageOperation(
        IObjectStorage & object_storage_,
        IMetadataStorage & metadata_storage_,
        IObjectStorage & destination_object_storage_,
        const ReadSettings & read_settings_,
        const WriteSettings & write_settings_,
        const std::string & from_path_,
        const std::string & to_path_)
        : IDiskObjectStorageOperation(object_storage_, metadata_storage_)
        , read_settings(read_settings_)
        , write_settings(write_settings_)
        , from_path(from_path_)
        , to_path(to_path_)
        , destination_object_storage(destination_object_storage_)
    {}

    std::string getInfoForLog() const override
    {
        return fmt::format("CopyFileObjectStorageOperation (path_from: {}, path_to: {})", from_path, to_path);
    }

    void execute(MetadataTransactionPtr tx) override
    {
        tx->createEmptyMetadataFile(to_path);
        auto source_blobs = metadata_storage.getStorageObjects(from_path); /// Full paths

        for (const auto & object_from : source_blobs)
        {
            auto object_key = destination_object_storage.generateObjectKeyForPath(to_path, std::nullopt /* key_prefix */);
            auto object_to = StoredObject(object_key.serialize());

            object_storage.copyObjectToAnotherObjectStorage(object_from, object_to,read_settings,write_settings, destination_object_storage);

            tx->addBlobToMetadata(to_path, object_key, object_from.bytes_size);

            created_objects.push_back(object_to);
        }
    }

    void undo() override
    {
         destination_object_storage.removeObjectsIfExist(created_objects);
    }

    void finalize() override
    {
    }
};

struct TruncateFileObjectStorageOperation final : public IDiskObjectStorageOperation
{
    std::string path;
    size_t size;

    TruncateFileOperationOutcomePtr truncate_outcome;

    TruncateFileObjectStorageOperation(
        IObjectStorage & object_storage_,
        IMetadataStorage & metadata_storage_,
        const std::string & path_,
        size_t size_)
        : IDiskObjectStorageOperation(object_storage_, metadata_storage_)
        , path(path_)
        , size(size_)
    {}

    std::string getInfoForLog() const override
    {
        return fmt::format("TruncateFileObjectStorageOperation (path: {}, size: {})", path, size);
    }

    void execute(MetadataTransactionPtr tx) override
    {
        if (metadata_storage.existsFile(path))
            truncate_outcome = tx->truncateFile(path, size);
    }

    void undo() override
    {

    }

    void finalize() override
    {
        if (!truncate_outcome)
            return;

        if (!truncate_outcome->objects_to_remove.empty())
            object_storage.removeObjectsIfExist(truncate_outcome->objects_to_remove);
    }
};

struct CreateEmptyFileObjectStorageOperation final : public IDiskObjectStorageOperation
{
    StoredObject object;

    CreateEmptyFileObjectStorageOperation(
        IObjectStorage & object_storage_,
        IMetadataStorage & metadata_storage_,
        const std::string & path_)
        : IDiskObjectStorageOperation(object_storage_, metadata_storage_)
    {
        const auto key = object_storage.generateObjectKeyForPath(path_, std::nullopt);
        object = StoredObject(key.serialize(), path_, /* file_size */0);
    }

    std::string getInfoForLog() const override
    {
        return fmt::format("CreateEmptyFileObjectStorageOperation (remote path: {}, local path: {})", object.remote_path, object.local_path);
    }

    void execute(MetadataTransactionPtr /* tx */) override
    {
        auto buf = object_storage.writeObject(object, WriteMode::Rewrite);
        buf->finalize();
    }

    void undo() override
    {
        object_storage.removeObjectIfExists(object);
    }

    void finalize() override {}
};

}

void DiskObjectStorageTransaction::createDirectory(const std::string & path)
{
    operations_to_execute.emplace_back(
        std::make_unique<PureMetadataObjectStorageOperation>(object_storage, metadata_storage, [path](MetadataTransactionPtr tx)
        {
            tx->createDirectory(path);
        }));
}

void DiskObjectStorageTransaction::createDirectories(const std::string & path)
{
    operations_to_execute.emplace_back(
        std::make_unique<PureMetadataObjectStorageOperation>(object_storage, metadata_storage, [path](MetadataTransactionPtr tx)
        {
            tx->createDirectoryRecursive(path);
        }));
}


void DiskObjectStorageTransaction::moveDirectory(const std::string & from_path, const std::string & to_path)
{
    operations_to_execute.emplace_back(
        std::make_unique<PureMetadataObjectStorageOperation>(object_storage, metadata_storage, [from_path, to_path](MetadataTransactionPtr tx)
        {
            tx->moveDirectory(from_path, to_path);
        }));
}

void DiskObjectStorageTransaction::moveFile(const String & from_path, const String & to_path)
{
     operations_to_execute.emplace_back(
        std::make_unique<PureMetadataObjectStorageOperation>(object_storage, metadata_storage, [from_path, to_path](MetadataTransactionPtr tx)
        {
            tx->moveFile(from_path, to_path);
        }));
}

void DiskObjectStorageTransaction::truncateFile(const String & path, size_t size)
{
    operations_to_execute.emplace_back(
        std::make_unique<TruncateFileObjectStorageOperation>(object_storage, metadata_storage, path, size)
    );
}

void DiskObjectStorageTransaction::replaceFile(const std::string & from_path, const std::string & to_path)
{
    auto operation = std::make_unique<ReplaceFileObjectStorageOperation>(object_storage, metadata_storage, from_path, to_path);
    operations_to_execute.emplace_back(std::move(operation));
}

void DiskObjectStorageTransaction::clearDirectory(const std::string & path)
{
    for (auto it = metadata_storage.iterateDirectory(path); it->isValid(); it->next())
    {
        if (metadata_storage.existsFile(it->path()))
            removeFile(it->path());
    }
}

void DiskObjectStorageTransaction::removeFile(const std::string & path)
{
    removeSharedFile(path, false);
}

void DiskObjectStorageTransaction::removeSharedFile(const std::string & path, bool keep_shared_data)
{
    auto operation = std::make_unique<RemoveObjectStorageOperation>(object_storage, metadata_storage, path, keep_shared_data, false);
    operations_to_execute.emplace_back(std::move(operation));
}

void DiskObjectStorageTransaction::removeSharedRecursive(
    const std::string & path, bool keep_all_shared_data, const NameSet & file_names_remove_metadata_only)
{
    auto operation = std::make_unique<RemoveRecursiveObjectStorageOperation>(
        object_storage, metadata_storage, path, keep_all_shared_data, file_names_remove_metadata_only);
    operations_to_execute.emplace_back(std::move(operation));
}

void DiskObjectStorageTransaction::removeSharedFileIfExists(const std::string & path, bool keep_shared_data)
{
    auto operation = std::make_unique<RemoveObjectStorageOperation>(object_storage, metadata_storage, path, keep_shared_data, true);
    operations_to_execute.emplace_back(std::move(operation));
}

void DiskObjectStorageTransaction::removeDirectory(const std::string & path)
{
    operations_to_execute.emplace_back(
        std::make_unique<PureMetadataObjectStorageOperation>(object_storage, metadata_storage, [path](MetadataTransactionPtr tx)
        {
            tx->removeDirectory(path);
        }));
}


void DiskObjectStorageTransaction::removeRecursive(const std::string & path)
{
    removeSharedRecursive(path, false, {});
}

void DiskObjectStorageTransaction::removeFileIfExists(const std::string & path)
{
    removeSharedFileIfExists(path, false);
}


void DiskObjectStorageTransaction::removeSharedFiles(
    const RemoveBatchRequest & files, bool keep_all_batch_data, const NameSet & file_names_remove_metadata_only)
{
    auto operation = std::make_unique<RemoveManyObjectStorageOperation>(object_storage, metadata_storage, files, keep_all_batch_data, file_names_remove_metadata_only);
    operations_to_execute.emplace_back(std::move(operation));
}

std::unique_ptr<WriteBufferFromFileBase> DiskObjectStorageTransaction::writeFile( /// NOLINT
    const std::string & path,
    size_t buf_size,
    WriteMode mode,
    const WriteSettings & settings,
    bool autocommit)
{
    auto object_key = object_storage.generateObjectKeyForPath(path, std::nullopt /* key_prefix */);
    std::optional<ObjectAttributes> object_attributes;

    /// Does metadata_storage support empty files without actual blobs in the object_storage?
    const bool do_not_write_empty_blob = metadata_storage.supportsEmptyFilesWithoutBlobs();

    /// Seems ok
    auto object = StoredObject(object_key.serialize(), path);
    std::function<void(size_t count)> create_metadata_callback;

    if (autocommit)
    {
        create_metadata_callback = [tx = shared_from_this(), mode, path, key_ = std::move(object_key), do_not_write_empty_blob](size_t count)
        {
            if (mode == WriteMode::Rewrite)
            {
                /// Otherwise we will produce lost blobs which nobody points to
                /// WriteOnce storages are not affected by the issue
                if (!tx->object_storage.isPlain())
                {
                    auto maybe_blobs = tx->metadata_transaction->tryGetBlobsFromTransactionIfExists(path);
                    if (maybe_blobs.has_value())
                        tx->object_storage.removeObjectsIfExist(*maybe_blobs);
                    /// NOTE: In the best case it would be better to remove file from metadata transaction
                    /// as well, nevertheless it will be overridden in createMetadataFilePath
                }

                if (do_not_write_empty_blob && count == 0)
                {
                    LoggerPtr logger = getLogger("DiskObjectStorageTransaction");
                    LOG_TRACE(logger, "Skipping writing empty blob for path {}, key {}", path, key_.serialize());
                    tx->metadata_transaction->createEmptyMetadataFile(path);
                }
                else
                    tx->metadata_transaction->createMetadataFile(path, key_, count);
            }
            else
            {
                /// Even if do_not_write_empty_blob and size is 0, we still need to add metadata just to make sure that a file gets created if this is the 1st append
                tx->metadata_transaction->addBlobToMetadata(path, key_, count);
            }

            tx->metadata_transaction->commit();
        };
    }
    else
    {
        auto write_operation = std::make_shared<WriteFileObjectStorageOperation>(object_storage, metadata_storage, object);

        create_metadata_callback = [object_storage_tx = shared_from_this(), write_op = write_operation, mode, path, key_ = std::move(object_key), do_not_write_empty_blob](size_t count)
        {
            /// This callback called in WriteBuffer finalize method -- only there we actually know
            /// how many bytes were written. We don't control when this finalize method will be called
            /// so here we just modify operation itself, but don't execute anything (and don't modify metadata transaction).
            /// Otherwise it's possible to get reorder of operations, like:
            /// tx->createDirectory(xxx) -- will add metadata operation in execute
            /// buf1 = tx->writeFile(xxx/yyy.bin)
            /// buf2 = tx->writeFile(xxx/zzz.bin)
            /// ...
            /// buf1->finalize() // shouldn't do anything with metadata operations, just memoize what to do
            /// tx->commit()
            /// NOTE: we should not take shared_ptr, only raw pointer, because transaction object always lives
            /// longer than buffer with this callback. Otherwise we can get cyclic shared_ptr.
            write_op->setOnExecute([object_storage_tx_raw_ptr = object_storage_tx.get(), mode, path, key_, count, do_not_write_empty_blob](MetadataTransactionPtr tx)
            {
                if (mode == WriteMode::Rewrite)
                {
                    /// Otherwise we will produce lost blobs which nobody points to
                    /// WriteOnce storages are not affected by the issue
                    if (!object_storage_tx_raw_ptr->object_storage.isPlain())
                    {
                        auto maybe_blobs = tx->tryGetBlobsFromTransactionIfExists(path);
                        if (maybe_blobs.has_value())
                            object_storage_tx_raw_ptr->object_storage.removeObjectsIfExist(*maybe_blobs);
                        /// NOTE: In the best case it would be better to remove file from metadata transaction
                        /// as well, nevertheless it will be overridden in createMetadataFilePath
                    }

                    if (do_not_write_empty_blob && count == 0)
                    {
                        LoggerPtr logger = getLogger("DiskObjectStorageTransaction");
                        LOG_TRACE(logger, "Skipping writing empty blob for path {}, key {}", path, key_.serialize());
                        tx->createEmptyMetadataFile(path);
                    }
                    else
                        tx->createMetadataFile(path, key_, count);
                }
                else
                {
                    /// Even if do_not_write_empty_blob and size is 0, we still need to add metadata just to make sure that a file gets created if this is the 1st append
                    tx->addBlobToMetadata(path, key_, count);
                }
            });
        };

        operations_to_execute.emplace_back(std::move(write_operation));
    }

    [[maybe_unused]] bool use_distributed_cache = false;
    size_t use_buffer_size = buf_size;
#if ENABLE_DISTRIBUTED_CACHE
    use_distributed_cache = settings.write_through_distributed_cache
        && DistributedCache::Registry::instance().isReady(settings.distributed_cache_settings.read_only_from_current_az);
    if (use_distributed_cache && settings.distributed_cache_settings.write_through_cache_buffer_size)
        use_buffer_size = settings.distributed_cache_settings.write_through_cache_buffer_size;
#endif

    auto impl = object_storage.writeObject(
        object,
        /// We always use mode Rewrite because we simulate append using metadata and different files
        WriteMode::Rewrite,
        object_attributes,
        use_buffer_size,
        settings);

#if ENABLE_DISTRIBUTED_CACHE
    if (use_distributed_cache)
    {
        auto connection_info = object_storage.getConnectionInfo();
        if (connection_info)
        {
            auto global_context = Context::getGlobalContextInstance();
            auto query_context = CurrentThread::isInitialized() ? CurrentThread::get().getQueryContext() : nullptr;

            auto timeouts = ConnectionTimeouts::getTCPTimeoutsWithoutFailover(
                query_context ? query_context->getSettingsRef() : global_context->getSettingsRef());

            impl = std::make_unique<WriteBufferFromDistributedCache>(
                path,
                object,
                settings,
                connection_info,
                std::move(impl),
                std::move(timeouts));
        }
    }
#endif

    return std::make_unique<WriteBufferWithFinalizeCallback>(
        std::move(impl), std::move(create_metadata_callback), object.remote_path, do_not_write_empty_blob);
}


void DiskObjectStorageTransaction::writeFileUsingBlobWritingFunction(
    const String & path, WriteMode mode, WriteBlobFunction && write_blob_function)
{
    /// This function is a simplified and adapted version of DiskObjectStorageTransaction::writeFile().
    auto object_key = object_storage.generateObjectKeyForPath(path, std::nullopt /* key_prefix */);
    std::optional<ObjectAttributes> object_attributes;

    /// seems ok
    auto object = StoredObject(object_key.serialize(), path);
    auto write_operation = std::make_unique<WriteFileObjectStorageOperation>(object_storage, metadata_storage, object);

    operations_to_execute.emplace_back(std::move(write_operation));

    /// See DiskObjectStorage::getBlobPath().
    Strings blob_path;
    blob_path.reserve(2);
    blob_path.emplace_back(object.remote_path);
    String objects_namespace = object_storage.getObjectsNamespace();
    if (!objects_namespace.empty())
        blob_path.emplace_back(objects_namespace);

    /// We always use mode Rewrite because we simulate append using metadata and different files
    size_t object_size = std::move(write_blob_function)(blob_path, WriteMode::Rewrite, object_attributes);

    /// Create metadata (see create_metadata_callback in DiskObjectStorageTransaction::writeFile()).
    if (mode == WriteMode::Rewrite)
    {
        /// Otherwise we will produce lost blobs which nobody points to
        /// WriteOnce storages are not affected by the issue
        if (!object_storage.isPlain() && metadata_storage.existsFile(path))
            object_storage.removeObjectsIfExist(metadata_storage.getStorageObjects(path));

        metadata_transaction->createMetadataFile(path, std::move(object_key), object_size);
    }
    else
        metadata_transaction->addBlobToMetadata(path, std::move(object_key), object_size);
}


void DiskObjectStorageTransaction::createHardLink(const std::string & src_path, const std::string & dst_path)
{
    operations_to_execute.emplace_back(
        std::make_unique<PureMetadataObjectStorageOperation>(object_storage, metadata_storage, [src_path, dst_path](MetadataTransactionPtr tx)
        {
            tx->createHardLink(src_path, dst_path);
        }));
}

void DiskObjectStorageTransaction::setReadOnly(const std::string & path)
{
    operations_to_execute.emplace_back(
        std::make_unique<PureMetadataObjectStorageOperation>(object_storage, metadata_storage, [path](MetadataTransactionPtr tx)
        {
            tx->setReadOnly(path);
        }));
}

void DiskObjectStorageTransaction::setLastModified(const std::string & path, const Poco::Timestamp & timestamp)
{
    operations_to_execute.emplace_back(
        std::make_unique<PureMetadataObjectStorageOperation>(object_storage, metadata_storage, [path, timestamp](MetadataTransactionPtr tx)
        {
            tx->setLastModified(path, timestamp);
        }));
}

void DiskObjectStorageTransaction::chmod(const String & path, mode_t mode)
{
    operations_to_execute.emplace_back(
        std::make_unique<PureMetadataObjectStorageOperation>(object_storage, metadata_storage, [path, mode](MetadataTransactionPtr tx)
        {
            tx->chmod(path, mode);
        }));
}

void DiskObjectStorageTransaction::createFile(const std::string & path)
{
    if (object_storage.isPlain() && !object_storage.isWriteOnce())
    {
        operations_to_execute.emplace_back(
            std::make_unique<CreateEmptyFileObjectStorageOperation>(object_storage, metadata_storage, path));
    }

    operations_to_execute.emplace_back(
        std::make_unique<PureMetadataObjectStorageOperation>(object_storage, metadata_storage, [path](MetadataTransactionPtr tx)
        {
            tx->createEmptyMetadataFile(path);
        }));
}

void DiskObjectStorageTransaction::copyFile(const std::string & from_file_path, const std::string & to_file_path, const ReadSettings & read_settings, const WriteSettings & write_settings)
{
    operations_to_execute.emplace_back(std::make_unique<CopyFileObjectStorageOperation>(
        object_storage, metadata_storage, object_storage, read_settings, write_settings, from_file_path, to_file_path));
}

void MultipleDisksObjectStorageTransaction::copyFile(const std::string & from_file_path, const std::string & to_file_path, const ReadSettings & read_settings, const WriteSettings & write_settings)
{
    operations_to_execute.emplace_back(std::make_unique<CopyFileObjectStorageOperation>(
        object_storage, metadata_storage, destination_object_storage, read_settings, write_settings, from_file_path, to_file_path));
}

void DiskObjectStorageTransaction::commit(const TransactionCommitOptionsVariant & options)
{
    for (size_t i = 0; i < operations_to_execute.size(); ++i)
    {
        try
        {
            operations_to_execute[i]->execute(metadata_transaction);
        }
        catch (...)
        {
            tryLogCurrentException(
                getLogger("DiskObjectStorageTransaction"),
                fmt::format("An error occurred while executing transaction's operation #{} ({})", i, operations_to_execute[i]->getInfoForLog()));

            for (int64_t j = i; j >= 0; --j)
            {
                try
                {
                    operations_to_execute[j]->undo();
                }
                catch (...)
                {
                    tryLogCurrentException(
                        getLogger("DiskObjectStorageTransaction"),
                        fmt::format("An error occurred while undoing transaction's operation #{}", i));

                    throw;
                }
            }
            throw;
        }
    }

    try
    {
        metadata_transaction->commit(options);
    }
    catch (...)
    {
        if (needRollbackBlobs(options))
            undo();

        throw;
    }

    for (const auto & operation : operations_to_execute)
        operation->finalize();

    operations_to_execute.clear();

    is_committed = true;
}


TransactionCommitOutcomeVariant DiskObjectStorageTransaction::tryCommit(const TransactionCommitOptionsVariant & options)
{
    for (size_t i = 0; i < operations_to_execute.size(); ++i)
    {
        try
        {
            operations_to_execute[i]->execute(metadata_transaction);
        }
        catch (Exception & ex)
        {
            /// Reset metadata transaction, it will be refilled in operations_to_execute[i]->execute on the next retry if needed
            metadata_transaction = metadata_storage.createTransaction();

            ex.addMessage(fmt::format("While executing operation #{}", i));

            if (needRollbackBlobs(options))
            {
                for (int64_t j = i; j >= 0; --j)
                {
                    try
                    {
                        operations_to_execute[j]->undo();
                    }
                    catch (Exception & rollback_ex)
                    {
                        rollback_ex.addMessage(fmt::format("While undoing operation #{}", i));
                        throw;
                    }
                }
            }

            throw;
        }
    }

    TransactionCommitOutcomeVariant outcome = metadata_transaction->tryCommit(options);
    try
    {
        fiu_do_on(FailPoints::smt_insert_fake_hardware_error,
        {
            auto * result = std::get_if<MetaInKeeperCommitOutcome>(&outcome);
            result->code = Coordination::Error::ZOPERATIONTIMEOUT;
        });

        if (!isSuccessfulOutcome(outcome))
        {
            /// Reset metadata transaction, it will be refilled in operations_to_execute[i]->execute on the next retry if needed
            metadata_transaction = metadata_storage.createTransaction();

            if (canRollbackBlobs(options, outcome))
            {
                undo();
            }

            return outcome;
        }
    }
    catch (...)
    {
        tryLogCurrentException(__PRETTY_FUNCTION__);
        undo();
        throw;
    }

    for (const auto & operation : operations_to_execute)
        operation->finalize();

    operations_to_execute.clear();

    is_committed = true;
    return outcome;
}

void DiskObjectStorageTransaction::undo()
{
    if (!is_committed)
    {
        for (const auto & operation : operations_to_execute | std::views::reverse)
            operation->undo();
    }

    operations_to_execute.clear();
}

}
