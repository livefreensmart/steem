#include <steem/utilities/persistent_storage.hpp>
#include <steem/utilities/rocksdb_proxy.hpp>

#include <fc/log/logger.hpp>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/utilities/options_util.h>
#include <rocksdb/slice.h>
#include <rocksdb/utilities/write_batch_with_index.h>

namespace steem { namespace utilities {

#define check_status(s) FC_ASSERT((s).ok(), "Data access failed: ${m}", ("m", (s).ToString()))

persistent_storage::persistent_storage( const std::string& _plugin_name, const storage_configuration_manager& _config_manager )
: write_buffer( storage, columnHandles ), plugin_name( _plugin_name ), config_manager( _config_manager )
{

}

persistent_storage::~persistent_storage()
{
   shutdown_db();
}

void persistent_storage::store_sequence_ids()
{
   for( auto& item : sequences )
   {
      Slice slice( item.first );
      PrimitiveTypeSlice<size_t> id( item.second );

      auto s = write_buffer.Put( slice, id );
      check_status( s );
   }
}

void persistent_storage::load_seq_identifiers()
{
   ReadOptions rOptions;

   auto load = [ this, &rOptions ]( rocksdb_types::key_value_pair& obj )
   {
      Slice slice( obj.first );

      std::string buffer;
      auto s = storage->Get(rOptions, slice, &buffer );
      check_status(s);
      obj.first = PrimitiveTypeSlice<size_t>::unpackSlice( buffer );

      ilog( "Loaded ${name}: ${o}", ( "name", obj.first )( "o", obj.second ) );
   };

   std::for_each( sequences.begin(), sequences.end(), load );
}

void persistent_storage::save_store_version()
{
   for( auto& item : version )
   {
      primitive_type_slice<uint32_t> slice( item.second );
      auto s = write_buffer.Put( Slice( item.first ), slice );
      check_status(s);
   }
}

void persistent_storage::verify_store_version()
{
   ReadOptions rOptions;

   auto verify = [ this, &rOptions ]( rocksdb_types::key_value_pair& obj )
   {
      std::string buffer;
      auto s = storage->Get(rOptions, obj.first, &buffer );
      check_status(s);
      const auto value = primitive_type_slice<uint32_t>::unpackSlice( buffer );
      FC_ASSERT( value == obj.second, "Store version mismatch" );
   };

   std::for_each( version.begin(), version.end(), verify );
}

void persistent_storage::flush_write_buffer( DB* _db )
{
   store_sequence_ids();

   DB* db = ( _db == nullptr )? storage.get() : _db;

   ::rocksdb::WriteOptions wOptions;
   auto s = db->Write( wOptions, write_buffer.GetWriteBatch() );
   check_status(s);
   write_buffer.Clear();
   collectedOps = 0;
}
  
bool persistent_storage::flush_storage()
{
   if( storage == nullptr )
      return false;

   /// If there are still not yet saved changes let's do it now.
   if( collectedOps != 0 )
      flush_write_buffer();

   ::rocksdb::FlushOptions fOptions;
   for( const auto& cf : columnHandles )
   {
      auto s = storage->Flush(fOptions, cf);
      check_status(s);
   }

   return true;
}

void persistent_storage::cleanup_column_handles()
{
   for( auto*h : columnHandles )
      delete h;

   columnHandles.clear();
}

bool persistent_storage::shutdown_db()
{
   //chain::util::disconnect_signal(_on_pre_apply_operation_connection);
   bool res = flush_storage();
   cleanup_column_handles();
   storage.reset();

   return res;
}

bool persistent_storage::create_db()
{
   rocksdb_types::ColumnDefinitions columnDefs;

   auto preparer = config_manager.get_column_definitions_preparer( plugin_name );
   preparer( true, columnDefs );

   DB* db = nullptr;
   auto strPath = config_manager.get_storage_path( plugin_name ).string();

   Options options;
   /// Optimize RocksDB. This is the easiest way to get RocksDB to perform well
   options.IncreaseParallelism();
   options.OptimizeLevelStyleCompaction();

   auto s = DB::OpenForReadOnly( options, strPath, columnDefs, &columnHandles, &db );

   if( s.ok() )
   {
      cleanup_column_handles();
      delete db;
      return true; /// DB does not need data import.
   }

   bool res;

   options.create_if_missing = true;
   s = DB::Open( options, strPath, &db );

   if( s.ok() )
   {
      columnDefs.clear();
      preparer( false, columnDefs );
      s = db->CreateColumnFamilies( columnDefs, &columnHandles );
      if( s.ok() )
      {
         ilog("RockDB column definitions created successfully.");

         version = config_manager.get_version( plugin_name );
         sequences = config_manager.get_sequences( plugin_name );

         save_store_version();
         /// Store initial values of Seq-IDs for held objects.
         flush_write_buffer( db );
         cleanup_column_handles();

         res = true;
      }
      else
      {
         elog("RocksDB can not create column definitions at location: `${p}'.\nReturned error: ${e}",
            ("p", strPath)("e", s.ToString()));

         res = false;
      }

      delete db;

      return res;
   }
   else
   {
      elog("RocksDB can not create storage at location: `${p}'.\nReturned error: ${e}",
         ("p", strPath)("e", s.ToString()));

      return false;
   }
}

bool persistent_storage::open_db()
{
   rocksdb_types::ColumnDefinitions columnDefs;

   auto preparer = config_manager.get_column_definitions_preparer( plugin_name );
   preparer( true, columnDefs );

   sequences = config_manager.get_sequences( plugin_name );
   version = config_manager.get_version( plugin_name );

   DB* storageDb = nullptr;
   auto strPath = config_manager.get_storage_path( plugin_name ).string();

   DBOptions db_options;
   Status status;

   if( config_manager.exist_config_file( plugin_name ) )
   {
      std::vector<ColumnFamilyDescriptor> loaded_cf_descs;
      auto load_status = LoadOptionsFromFile( config_manager.get_config_file( plugin_name ).string(), Env::Default(), &db_options, &loaded_cf_descs );
      assert( load_status.ok() );
   }

   /// Optimize RocksDB. This is the easiest way to get RocksDB to perform well
   db_options.IncreaseParallelism();

   status = DB::Open( db_options, strPath, columnDefs, &columnHandles, &storageDb );

   if( status.ok() )
   {
      ilog("RocksDB opened successfully storage at location: `${p}'.", ("p", strPath));

      storage.reset( storageDb );

      verify_store_version();
      load_seq_identifiers();

      return true;
   }
   else
   {
      elog("RocksDB cannot open database at location: `${p}'.\nReturned error: ${e}",
         ("p", strPath)("e", status.ToString()));

      return false;
   }
}

bool persistent_storage::action( std::function< bool() > call )
{
   try
   {
      return call();
   }
   catch(...)
   {
      return false;
   }
}

bool persistent_storage::create()
{
   return action( [ this ](){ return create_db(); } );
}

bool persistent_storage::open()
{
   return action( [ this ](){ return open_db(); } );
}

bool persistent_storage::flush()
{
   return action( [ this ](){ return flush_storage(); } );
}

bool persistent_storage::close()
{
   return action( [ this ](){ return shutdown_db(); } );
}

bool persistent_storage::is_opened()
{
   return storage != nullptr;
}

bool persistent_storage::is_closed()
{
   return storage == nullptr;
}

abstract_persistent_storage::ptrDB& persistent_storage::get_storage()
{
   assert( !storage );
   return storage;
}

} }
