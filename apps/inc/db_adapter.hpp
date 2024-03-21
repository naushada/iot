#ifndef __db_adapter_hpp__
#define __db_adapter_hpp__


#include <cstdint>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <sstream>
#include <mutex>
#include <chrono>

#include <mongocxx/pool.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/stdx.hpp>
#include <mongocxx/uri.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/bulk_write.hpp>

#include <mongocxx/gridfs/uploader.hpp>
#include <mongocxx/gridfs/downloader.hpp>
#include <mongocxx/gridfs/bucket.hpp>

#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/builder/stream/helpers.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/builder/stream/array.hpp>
#include <bsoncxx/stdx/string_view.hpp>
#include <bsoncxx/string/to_string.hpp>

class DbClient {
    public:
        DbClient(std::string uri);
        DbClient();
        ~DbClient();

        std::string get_database() const {
            return(mdbName);
        }

        void set_database(std::string dbName) {
            mdbName = dbName;
        }

        std::string get_uri() const {
            return(mURI);
        }

        void set_uri(std::string uri) {
            mURI = uri;
        }

        std::string upload_file(std::string fileName, std::string& content, std::uint32_t len);
        std::string download_file(std::string fileId);
        bool update_collection(std::string coll, std::string filter, std::string document);
        bool update_collectionEx(std::string coll, std::string filter, std::string document);
        bool delete_document(std::string coll, std::string shippingRecord);
        bool delete_documents(std::string collectionName, std::string doc);
        bool delete_documentEx(std::string coll, std::string shippingRecord);
        
        std::string create_document(std::string dbName, std::string coll, std::string accountRecord);
        std::string create_document(std::string collectionName, std::string doc);
        std::int32_t create_documents(std::string collectionName, std::string doc);
        std::int32_t create_bulk_document(std::string dbName, std::string coll, std::string doc);

        std::string create_documentEx(std::string coll, std::string accountRecord);
        std::string get_document(std::string collectionName, std::string query, std::string fieldProjection);
        std::string get_documents(std::string collectionName, std::string query, std::string fieldProjection);
        std::string get_documents(std::string collectionName, std::string fieldProjection);
        std::string get_documentEx(std::string collectionName, std::string query, std::string fieldProjection);
        std::string get_documentsEx(std::string collectionName, std::string query, std::string fieldProjection);
        std::string get_byOID(std::string collection, std::string projection, std::string oid);
        std::string get_documentList(std::string collectionName, std::string query, std::string fieldProjection);
        
        std::string get_documentListEx(std::string collectionName, std::string query, std::string fieldProjection);
        std::int32_t create_bulk_documentEx(std::string collectionName, std::string doc);
        std::uint32_t from_json_array_to_vector(const std::string json_obj, const std::string key, std::vector<std::string>& vec_out);
        std::uint32_t from_json_element_to_string(const std::string json_obj, const std::string key, std::string& str_out);
        std::uint32_t from_json_object_to_map(const std::string json_obj, const std::string key, std::vector<std::tuple<std::string, std::string>>& out);

    private:
        std::string mURI;
        std::string mdbName;
        /* Pool of db connections */
        std::unique_ptr<mongocxx::pool> mConnPool;
        std::unique_ptr<mongocxx::instance> mInstance;
};











#endif /* __db_adapter_hpp__ */