#include <Storages/StorageFactory.h>
#include <Storages/StorageURL.h>

#include <Interpreters/Context.h>
#include <Interpreters/evaluateConstantExpression.h>
#include <Parsers/ASTLiteral.h>

#include <IO/ReadHelpers.h>
#include <IO/ReadWriteBufferFromHTTP.h>
#include <IO/WriteBufferFromHTTP.h>
#include <IO/WriteHelpers.h>

#include <Formats/FormatFactory.h>

#include <DataStreams/IBlockOutputStream.h>
#include <DataStreams/IBlockInputStream.h>
#include <DataStreams/AddingDefaultsBlockInputStream.h>

#include <Poco/Net/HTTPRequest.h>


namespace DB
{
namespace ErrorCodes
{
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int UNACCEPTABLE_URL;
}

IStorageURLBase::IStorageURLBase(
    const Poco::URI & uri_,
    const Context & context_,
    const std::string & database_name_,
    const std::string & table_name_,
    const String & format_name_,
    const ColumnsDescription & columns_,
    const ConstraintsDescription & constraints_,
    const String & compression_method_)
    : uri(uri_), context_global(context_), compression_method(compression_method_), format_name(format_name_), table_name(table_name_), database_name(database_name_)
{
    context_global.getRemoteHostFilter().checkURL(uri);
    setColumns(columns_);
    setConstraints(constraints_);
}

namespace
{
    class StorageURLBlockInputStream : public IBlockInputStream
    {
    public:
        StorageURLBlockInputStream(const Poco::URI & uri,
            const std::string & method,
            std::function<void(std::ostream &)> callback,
            const String & format,
            const String & name_,
            const Block & sample_block,
            const Context & context,
            UInt64 max_block_size,
            const ConnectionTimeouts & timeouts,
            const CompressionMethod compression_method)
            : name(name_)
        {
            read_buf = getReadBuffer<ReadWriteBufferFromHTTP>(
                compression_method,
                uri,
                method,
                callback,
                timeouts,
                context.getSettingsRef().max_http_get_redirects,
                Poco::Net::HTTPBasicCredentials{},
                DBMS_DEFAULT_BUFFER_SIZE,
                ReadWriteBufferFromHTTP::HTTPHeaderEntries{},
                context.getRemoteHostFilter());

            reader = FormatFactory::instance().getInput(format, *read_buf, sample_block, context, max_block_size);
        }

        String getName() const override
        {
            return name;
        }

        Block readImpl() override
        {
            return reader->read();
        }

        Block getHeader() const override
        {
            return reader->getHeader();
        }

        void readPrefixImpl() override
        {
            reader->readPrefix();
        }

        void readSuffixImpl() override
        {
            reader->readSuffix();
        }

    private:
        String name;
        std::unique_ptr<ReadBuffer> read_buf;
        BlockInputStreamPtr reader;
    };

    class StorageURLBlockOutputStream : public IBlockOutputStream
    {
    public:
        StorageURLBlockOutputStream(const Poco::URI & uri,
            const String & format,
            const Block & sample_block_,
            const Context & context,
            const ConnectionTimeouts & timeouts,
            const CompressionMethod compression_method)
            : sample_block(sample_block_)
        {
            write_buf = getWriteBuffer<WriteBufferFromHTTP>(compression_method, uri, Poco::Net::HTTPRequest::HTTP_POST, timeouts);
            writer = FormatFactory::instance().getOutput(format, *write_buf, sample_block, context);
        }

        Block getHeader() const override
        {
            return sample_block;
        }

        void write(const Block & block) override
        {
            writer->write(block);
        }

        void writePrefix() override
        {
            writer->writePrefix();
        }

        void writeSuffix() override
        {
            writer->writeSuffix();
            writer->flush();
            write_buf->finalize();
        }

    private:
        Block sample_block;
        std::unique_ptr<WriteBuffer> write_buf;
        BlockOutputStreamPtr writer;
    };
}


std::string IStorageURLBase::getReadMethod() const
{
    return Poco::Net::HTTPRequest::HTTP_GET;
}

std::vector<std::pair<std::string, std::string>> IStorageURLBase::getReadURIParams(const Names & /*column_names*/,
    const SelectQueryInfo & /*query_info*/,
    const Context & /*context*/,
    QueryProcessingStage::Enum & /*processed_stage*/,
    size_t /*max_block_size*/) const
{
    return {};
}

std::function<void(std::ostream &)> IStorageURLBase::getReadPOSTDataCallback(const Names & /*column_names*/,
    const SelectQueryInfo & /*query_info*/,
    const Context & /*context*/,
    QueryProcessingStage::Enum & /*processed_stage*/,
    size_t /*max_block_size*/) const
{
    return nullptr;
}


BlockInputStreams IStorageURLBase::read(const Names & column_names,
    const SelectQueryInfo & query_info,
    const Context & context,
    QueryProcessingStage::Enum processed_stage,
    size_t max_block_size,
    unsigned /*num_streams*/)
{
    auto request_uri = uri;
    auto params = getReadURIParams(column_names, query_info, context, processed_stage, max_block_size);
    for (const auto & [param, value] : params)
        request_uri.addQueryParameter(param, value);

    BlockInputStreamPtr block_input = std::make_shared<StorageURLBlockInputStream>(request_uri,
        getReadMethod(),
        getReadPOSTDataCallback(column_names, query_info, context, processed_stage, max_block_size),
        format_name,
        getName(),
        getHeaderBlock(column_names),
        context,
        max_block_size,
        ConnectionTimeouts::getHTTPTimeouts(context),
        IStorage::chooseCompressionMethod(request_uri.getPath(), compression_method));

    auto column_defaults = getColumns().getDefaults();
    if (column_defaults.empty())
        return {block_input};
    return {std::make_shared<AddingDefaultsBlockInputStream>(block_input, column_defaults, context)};
}

void IStorageURLBase::rename(const String & /*new_path_to_db*/, const String & new_database_name, const String & new_table_name, TableStructureWriteLockHolder &)
{
    table_name = new_table_name;
    database_name = new_database_name;
}

BlockOutputStreamPtr IStorageURLBase::write(const ASTPtr & /*query*/, const Context & /*context*/)
{
    return std::make_shared<StorageURLBlockOutputStream>(
        uri, format_name, getSampleBlock(), context_global,
        ConnectionTimeouts::getHTTPTimeouts(context_global),
        IStorage::chooseCompressionMethod(uri.toString(), compression_method));
}

void registerStorageURL(StorageFactory & factory)
{
    factory.registerStorage("URL", [](const StorageFactory::Arguments & args)
    {
        ASTs & engine_args = args.engine_args;

        if (engine_args.size() != 2 && engine_args.size() != 3)
            throw Exception(
                "Storage URL requires 2 or 3 arguments: url, name of used format and optional compression method.", ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        engine_args[0] = evaluateConstantExpressionOrIdentifierAsLiteral(engine_args[0], args.local_context);

        String url = engine_args[0]->as<ASTLiteral &>().value.safeGet<String>();
        Poco::URI uri(url);

        engine_args[1] = evaluateConstantExpressionOrIdentifierAsLiteral(engine_args[1], args.local_context);

        String format_name = engine_args[1]->as<ASTLiteral &>().value.safeGet<String>();

        String compression_method;
        if (engine_args.size() == 3)
        {
            engine_args[2] = evaluateConstantExpressionOrIdentifierAsLiteral(engine_args[2], args.local_context);
            compression_method = engine_args[2]->as<ASTLiteral &>().value.safeGet<String>();
        } else compression_method = "auto";

        return StorageURL::create(
            uri,
            args.database_name, args.table_name,
            format_name,
            args.columns, args.constraints, args.context,
            compression_method);
    });
}
}
