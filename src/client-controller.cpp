/*
Copyright (c) 2013, David C Horton

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/
#include <iostream>

#include <boost/bind.hpp>
#include <boost/tokenizer.hpp>
#include <boost/foreach.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/asio.hpp>

#include "client-controller.hpp"
#include "controller.hpp"

namespace drachtio {
     
    ClientController::ClientController( DrachtioController* pController, string& address, unsigned int port ) :
        m_pController( pController ),
        m_endpoint(  boost::asio::ip::tcp::v4(), port ),
        m_acceptor( m_ioservice, m_endpoint ) {
            
        srand (time(NULL));    
        boost::thread t(&ClientController::threadFunc, this) ;
        m_thread.swap( t ) ;
            
        this->start_accept() ;
    }
    ClientController::~ClientController() {
        this->stop() ;
    }
    void ClientController::threadFunc() {
        
        DR_LOG(log_debug) << "Client controller thread id: " << boost::this_thread::get_id()  ;
         
        /* to make sure the event loop doesn't terminate when there is no work to do */
        boost::asio::io_service::work work(m_ioservice);
        
        for(;;) {
            
            try {
                DR_LOG(log_notice) << "ClientController: io_service run loop started"  ;
                m_ioservice.run() ;
                DR_LOG(log_notice) << "ClientController: io_service run loop ended normally"  ;
                break ;
            }
            catch( std::exception& e) {
                DR_LOG(log_error) << "Error in event thread: " << string( e.what() )  ;
                break ;
            }
        }
    }
    void ClientController::join( client_ptr client ) {
        m_clients.insert( client ) ;
        client_weak_ptr p( client ) ;
        DR_LOG(log_debug) << "Added client, count of connected clients is now: " << m_clients.size()  ;       
    }
    void ClientController::leave( client_ptr client ) {
        m_clients.erase( client ) ;
        DR_LOG(log_debug) << "Removed client, count of connected clients is now: " << m_clients.size()  ;
    }
    void ClientController::addNamedService( client_ptr client, string& strAppName ) {
        //TODO: should we be locking here?  need to review entire locking strategy for this class
        client_weak_ptr p( client ) ;
        m_services.insert( map_of_services::value_type(strAppName,p)) ;       
    }

	void ClientController::start_accept() {
		client_ptr new_session( new Client( m_ioservice, *this ) ) ;
		m_acceptor.async_accept( new_session->socket(), boost::bind(&ClientController::accept_handler, this, new_session, boost::asio::placeholders::error));
    }
	void ClientController::accept_handler( client_ptr session, const boost::system::error_code& ec) {
        if(!ec) session->start() ;
        start_accept(); 
    }
    bool ClientController::wants_requests( client_ptr client, const string& verb ) {
        RequestSpecifier spec( client ) ;
        boost::lock_guard<boost::mutex> l( m_lock ) ;
        m_request_types.insert( map_of_request_types::value_type(verb, spec)) ;  
        DR_LOG(log_debug) << "Added client for " << verb << " requests"  ;

        //initialize the offset if this is the first client registering for that verb
        map_of_request_type_offsets::iterator it = m_map_of_request_type_offsets.find( verb ) ;
        if( m_map_of_request_type_offsets.end() == it ) m_map_of_request_type_offsets.insert(map_of_request_type_offsets::value_type(verb,0)) ;

        //TODO: validate the verb is supported
        return true ;  
    }

    client_ptr ClientController::selectClientForRequestOutsideDialog( const char* keyword ) {
        string method_name = keyword ;
        transform(method_name.begin(), method_name.end(), method_name.begin(), ::tolower);

        /* round robin select a client that has registered for this request type */
        boost::lock_guard<boost::mutex> l( m_lock ) ;
        client_ptr client ;
        string matchId ;
        pair<map_of_request_types::iterator,map_of_request_types::iterator> pair = m_request_types.equal_range( method_name ) ;
        unsigned int nPossibles = std::distance( pair.first, pair.second ) ;
        if( 0 == nPossibles ) {
            DR_LOG(log_info) << "No connected clients found to handle incoming " << method_name << " request"  ;
           return client ;           
        }

        unsigned int nOffset = 0 ;
        map_of_request_type_offsets::const_iterator itOffset = m_map_of_request_type_offsets.find( method_name ) ;
        if( m_map_of_request_type_offsets.end() != itOffset ) {
            unsigned int i = itOffset->second;
            if( i < nPossibles ) nOffset = i ;
            else nOffset = 0;
        }
        DR_LOG(log_debug) << "ClientController::route_request_outside_dialog - there are " << nPossibles << 
            " possible clients, we are starting with offset " << nOffset  ;

        m_map_of_request_type_offsets.erase( itOffset ) ;
        m_map_of_request_type_offsets.insert(map_of_request_type_offsets::value_type(method_name, nOffset + 1)) ;

        unsigned int nTries = 0 ;
        do {
            map_of_request_types::iterator it = pair.first ;
            std::advance( it, nOffset) ;
            RequestSpecifier& spec = it->second ;
            client = spec.client() ;
            if( !client ) {
                DR_LOG(log_debug) << "Removing disconnected client while iterating"  ;
                m_request_types.erase( it ) ;
                pair = m_request_types.equal_range( method_name ) ;
                if( nOffset >= m_request_types.size() ) {
                    nOffset = m_request_types.size() - 1 ;
                }
                DR_LOG(log_debug) << "Offset has been set to " << nOffset << " size of range is " << m_request_types.size()  ;
            }
            else {
                DR_LOG(log_debug) << "Selected client at offset " << nOffset  ;                
            }
        } while( !client && ++nTries < nPossibles ) ;

        if( !client ) {
            DR_LOG(log_info) << "No clients found to handle incoming " << method_name << " request"  ;
            return client ;
        }
 
        return client ;
    }
    bool ClientController::route_ack_request_inside_dialog( const string& rawSipMsg, const SipMsgData_t& meta, nta_incoming_t* prack, 
        sip_t const *sip, const string& transactionId, const string& inviteTransactionId, const string& dialogId ) {
        client_ptr client = this->findClientForDialog( dialogId );
        if( !client ) {
            client = this->findClientForNetTransaction( inviteTransactionId );
            if( !client ) {
               DR_LOG(log_warning) << "ClientController::route_ack_request_inside_dialog - client managing dialog has disconnected: " << dialogId  ;            
                //TODO: try to find another client providing the same service
                return false ;
            }
        }

        m_ioservice.post( boost::bind(&Client::sendSipMessageToClient, client, transactionId, dialogId, rawSipMsg, meta) ) ;

        this->removeNetTransaction( inviteTransactionId ) ;
        DR_LOG(log_debug) << "ClientController::route_ack_request_inside_dialog - removed incoming invite transaction, map size is now: " << m_mapNetTransactions.size() << " request"  ;
 
        return true ;

    }
    bool ClientController::route_request_inside_invite( const string& rawSipMsg, const SipMsgData_t& meta, nta_incoming_t* irq, sip_t const *sip, 
        const string& transactionId, const string& dialogId  ) {
        //client_ptr client = this->findClientForDialog( dialogId );
        //if( !client ) {
            client_ptr client = this->findClientForNetTransaction( transactionId );
            if( !client ) {
                DR_LOG(log_warning) << "ClientController::route_response_inside_invite - client managing transaction has disconnected: " << transactionId  ;
                return false ;
            }
        //}
 
        DR_LOG(log_debug) << "ClientController::route_response_inside_invite - sending response to client"  ;
        m_ioservice.post( boost::bind(&Client::sendSipMessageToClient, client, transactionId, dialogId, rawSipMsg, meta) ) ;

        return true ;
    }

    bool ClientController::route_request_inside_dialog( const string& rawSipMsg, const SipMsgData_t& meta, nta_incoming_t* irq, sip_t const *sip, 
        const string& transactionId, const string& dialogId ) {
        client_ptr client = this->findClientForDialog( dialogId );
        if( !client ) {
            DR_LOG(log_warning) << "ClientController::route_request_inside_dialog - client managing dialog has disconnected: " << dialogId  ;
            
            //TODO: try to find another client providing the same service
            return false ;
        }
        this->addNetTransaction( client, transactionId ) ;
 
        m_ioservice.post( boost::bind(&Client::sendSipMessageToClient, client, transactionId, dialogId, rawSipMsg, meta) ) ;

        // if this is a BYE from the network, it ends the dialog 
        string method_name = sip->sip_request->rq_method_name ;
        if( 0 == method_name.compare("BYE") ) {
            removeDialog( dialogId ) ;
        }

        return true ;
    }

    bool ClientController::route_response_inside_transaction( const string& rawSipMsg, const SipMsgData_t& meta, nta_outgoing_t* orq, sip_t const *sip, 
        const string& transactionId, const string& dialogId ) {
        
        client_ptr client = this->findClientForAppTransaction( transactionId );
        if( !client ) {
            DR_LOG(log_warning) << "ClientController::route_response_inside_transaction - client managing transaction has disconnected: " << transactionId  ;
            return false ;
        }

        m_ioservice.post( boost::bind(&Client::sendSipMessageToClient, client, transactionId, dialogId, rawSipMsg, meta) ) ;

        string method_name = sip->sip_cseq->cs_method_name ;

        if( sip->sip_status->st_status >= 200 ) {
            removeAppTransaction( transactionId ) ;
        }

        if( 0 == method_name.compare("BYE") ) {
            removeDialog( dialogId ) ;
        }

        return true ;
    }
    
    void ClientController::addDialogForTransaction( const string& transactionId, const string& dialogId ) {
        boost::lock_guard<boost::mutex> l( m_lock ) ;
        mapId2Client::iterator it = m_mapNetTransactions.find( transactionId ) ;
        if( m_mapNetTransactions.end() != it ) {
            m_mapDialogs.insert( mapId2Client::value_type(dialogId, it->second ) ) ;
            DR_LOG(log_warning) << "ClientController::addDialogForTransaction - added dialog (uas), now tracking: " << 
                m_mapDialogs.size() << " dialogs and " << m_mapNetTransactions.size() << " net transactions"  ;
         }
        else {
            /* dialog will already exist if we received a reliable provisional response */
            mapId2Client::iterator itDialog = m_mapDialogs.find( dialogId ) ;
            if( m_mapDialogs.end() == itDialog ) {
                mapId2Client::iterator itApp = m_mapAppTransactions.find( transactionId ) ;
                if( m_mapAppTransactions.end() != itApp ) {
                    m_mapDialogs.insert( mapId2Client::value_type(dialogId, itApp->second ) ) ;
                    DR_LOG(log_warning) << "ClientController::addDialogForTransaction - added dialog (uac), now tracking: " << 
                        m_mapDialogs.size() << " dialogs and " << m_mapAppTransactions.size() << " app transactions"  ;
                }
                else {
                   DR_LOG(log_error) << "ClientController::addDialogForTransaction - transaction id " << transactionId << " not found"  ;
                    assert(false) ;                           
                }
            }
        }
        DR_LOG(log_debug) << "ClientController::addDialogForTransaction - transaction id " << transactionId << 
            " has associated dialog " << dialogId  ;

        client_ptr client = this->findClientForDialog_nolock( dialogId );
        if( !client ) {
            m_mapDialogs.erase( dialogId ) ;
            DR_LOG(log_warning) << "ClientController::addDialogForTransaction - client managing dialog has disconnected: " << dialogId  ;
            return  ;
        }
        else {
            string strAppName ;
            if( client->getAppName( strAppName ) ) {
                m_mapDialogId2Appname.insert( mapDialogId2Appname::value_type( dialogId, strAppName ) ) ;
                
                DR_LOG(log_debug) << "ClientController::addDialogForTransaction - dialog id " << dialogId << 
                    " has been established for client app " << strAppName << "; count of tracked dialogs is " << m_mapDialogId2Appname.size()  ;
            }
        }
    } 
    bool ClientController::sendRequestInsideDialog( client_ptr client, const string& clientMsgId, const string& dialogId, const string& startLine, 
        const string& headers, const string& body, string& transactionId ) {

        generateUuid( transactionId ) ;
        if( 0 != startLine.find("ACK") ) {
            addAppTransaction( client, transactionId ) ;
        }

        addApiRequest( client, clientMsgId )  ;
        bool rc = m_pController->getDialogController()->sendRequestInsideDialog( clientMsgId, dialogId, startLine, headers, body, transactionId) ;
        return rc ;
    }
    bool ClientController::sendRequestOutsideDialog( client_ptr client, const string& clientMsgId, const string& startLine, const string& headers, 
            const string& body, string& transactionId, string& dialogId ) {

        generateUuid( transactionId ) ;
        if( 0 != startLine.find("ACK") ) {
            addAppTransaction( client, transactionId ) ;
        }

        addApiRequest( client, clientMsgId )  ;
        bool rc = m_pController->getDialogController()->sendRequestOutsideDialog( clientMsgId, startLine, headers, body, transactionId, dialogId) ;
        return rc ;        
    }
    bool ClientController::respondToSipRequest( client_ptr client, const string& clientMsgId, const string& transactionId, const string& startLine, const string& headers, 
        const string& body ) {

        addApiRequest( client, clientMsgId )  ;
        bool rc = m_pController->getDialogController()->respondToSipRequest( clientMsgId, transactionId, startLine, headers, body ) ;
        return rc ;               
    }   
    bool ClientController::sendCancelRequest( client_ptr client, const string& clientMsgId, const string& transactionId, const string& startLine, const string& headers, 
        const string& body ) {

        addApiRequest( client, clientMsgId )  ;
        bool rc = m_pController->getDialogController()->sendCancelRequest( clientMsgId, transactionId, startLine, headers, body ) ;
        return rc ;               
    }
    bool ClientController::proxyRequest( client_ptr client, const string& clientMsgId, const string& transactionId, 
        bool recordRoute, bool fullResponse, bool followRedirects, bool simultaneous, const string& provisionalTimeout, 
        const string& finalTimeout, const vector<string>& vecDestination, const string& headers ) {
        addApiRequest( client, clientMsgId )  ;
        m_pController->getProxyController()->proxyRequest( clientMsgId, transactionId, recordRoute, fullResponse, followRedirects, 
            simultaneous, provisionalTimeout, finalTimeout, vecDestination, headers ) ;
        removeNetTransaction( transactionId ) ;
        return true;
    }
    bool ClientController::route_api_response( const string& clientMsgId, const string& responseText, const string& additionalResponseData ) {
       client_ptr client = this->findClientForApiRequest( clientMsgId );
        if( !client ) {
            removeApiRequest( clientMsgId ) ;
            DR_LOG(log_warning) << "ClientController::route_api_response - client that has sent the request has disconnected: " << clientMsgId  ;
            return false ;             
        }
        if( string::npos == additionalResponseData.find("|continue") ) {
            removeApiRequest( clientMsgId ) ;
        }
        m_ioservice.post( boost::bind(&Client::sendApiResponseToClient, client, clientMsgId, responseText, additionalResponseData) ) ;
        return true ;                
    }
    
    void ClientController::removeDialog( const string& dialogId ) {
        boost::lock_guard<boost::mutex> l( m_lock ) ;
        mapId2Client::iterator it = m_mapDialogs.find( dialogId ) ;
        if( m_mapDialogs.end() == it ) {
            DR_LOG(log_warning) << "ClientController::removeDialog - dialog not found: " << dialogId  ;
            return ;
        }
        m_mapDialogs.erase( it ) ;
        DR_LOG(log_info) << "ClientController::removeDialog - after removing dialogs count is now: " << m_mapDialogs.size()  ;
    }
    client_ptr ClientController::findClientForDialog( const string& dialogId ) {
        boost::lock_guard<boost::mutex> l( m_lock ) ;
        return findClientForDialog_nolock( dialogId ) ;
    }

    client_ptr ClientController::findClientForDialog_nolock( const string& dialogId ) {
        client_ptr client ;

        mapId2Client::iterator it = m_mapDialogs.find( dialogId ) ;
        if( m_mapDialogs.end() != it ) client = it->second.lock() ;

        // if that client is no longer connected, randomly select another client that is running that app 
        if( !client ) {
            mapDialogId2Appname::iterator it = m_mapDialogId2Appname.find( dialogId ) ;
            if( m_mapDialogId2Appname.end() != it ) {
                string appName = it->second ;
                DR_LOG(log_info) << "Attempting to find another client for app " << appName  ;

                pair<map_of_services::iterator,map_of_services::iterator> pair = m_services.equal_range( appName ) ;
                unsigned int nPossibles = std::distance( pair.first, pair.second ) ;
                if( 0 == nPossibles ) {
                   DR_LOG(log_warning) << "No other clients found for app " << appName  ;
                   return client ;
                }
                unsigned int nOffset = rand() % nPossibles ;
                unsigned int nTries = 0 ;
                do {
                    map_of_services::iterator itTemp = pair.first ;
                    std::advance( itTemp, nOffset) ;
                    client = itTemp->second.lock() ;
                    if( !client ) {
                        if( ++nOffset == nPossibles ) nOffset = 0 ;
                    }
                } while( !client && ++nTries < nPossibles ) ;

                if( !client ) DR_LOG(log_warning) << "No other connected clients found for app " << appName  ;
                else DR_LOG(log_info) << "Found alternative client for app " << appName << " " << nOffset << ":" << nPossibles  ;
            }
        }
        return client ;
    }

    client_ptr ClientController::findClientForAppTransaction( const string& transactionId ) {
        boost::lock_guard<boost::mutex> l( m_lock ) ;
        client_ptr client ;
        mapId2Client::iterator it = m_mapAppTransactions.find( transactionId ) ;
        if( m_mapAppTransactions.end() != it ) client = it->second.lock() ;
        return client ;
    }
    client_ptr ClientController::findClientForNetTransaction( const string& transactionId ) {
        boost::lock_guard<boost::mutex> l( m_lock ) ;
        client_ptr client ;
        mapId2Client::iterator it = m_mapNetTransactions.find( transactionId ) ;
        if( m_mapNetTransactions.end() != it ) client = it->second.lock() ;
        return client ;
    }
    client_ptr ClientController::findClientForApiRequest( const string& clientMsgId ) {
        boost::lock_guard<boost::mutex> l( m_lock ) ;
        client_ptr client ;
        mapId2Client::iterator it = m_mapApiRequests.find( clientMsgId ) ;
        if( m_mapApiRequests.end() != it ) client = it->second.lock() ;
        return client ;
    }
    void ClientController::removeAppTransaction( const string& transactionId ) {
        boost::lock_guard<boost::mutex> l( m_lock ) ;
        m_mapAppTransactions.erase( transactionId ) ;        
    }
    void ClientController::removeNetTransaction( const string& transactionId ) {
        boost::lock_guard<boost::mutex> l( m_lock ) ;
        m_mapNetTransactions.erase( transactionId ) ;        
        DR_LOG(log_debug) << "removeNetTransaction: transactionId " << transactionId << "; size: " << m_mapNetTransactions.size()  ;
    }
    void ClientController::removeApiRequest( const string& clientMsgId ) {
        boost::lock_guard<boost::mutex> l( m_lock ) ;
        m_mapApiRequests.erase( clientMsgId ) ;   
        DR_LOG(log_debug) << "removeApiRequest: clientMsgId " << clientMsgId << "; size: " << m_mapApiRequests.size()  ;
    }
    void ClientController::addAppTransaction( client_ptr client, const string& transactionId ) {
        boost::lock_guard<boost::mutex> l( m_lock ) ;
        m_mapAppTransactions.insert( make_pair( transactionId, client ) ) ;        
    }
    void ClientController::addNetTransaction( client_ptr client, const string& transactionId ) {
        boost::lock_guard<boost::mutex> l( m_lock ) ;
        m_mapNetTransactions.insert( make_pair( transactionId, client ) ) ;        
        DR_LOG(log_debug) << "addNetTransaction: transactionId " << transactionId << "; size: " << m_mapNetTransactions.size()  ;
    }
    void ClientController::addApiRequest( client_ptr client, const string& clientMsgId ) {
        boost::lock_guard<boost::mutex> l( m_lock ) ;
        m_mapApiRequests.insert( make_pair( clientMsgId, client ) ) ;        
        DR_LOG(log_debug) << "addApiRequest: clientMsgId " << clientMsgId << "; size: " << m_mapApiRequests.size()  ;
    }

    void ClientController::logStorageCount() {
        boost::lock_guard<boost::mutex> lock(m_lock) ;

        DR_LOG(log_debug) << "ClientController storage counts"  ;
        DR_LOG(log_debug) << "----------------------------------"  ;
        DR_LOG(log_debug) << "m_clients size:                                                  " << m_clients.size()  ;
        DR_LOG(log_debug) << "m_services size:                                                 " << m_services.size()  ;
        DR_LOG(log_debug) << "m_request_types size:                                            " << m_request_types.size()  ;
        DR_LOG(log_debug) << "m_map_of_request_type_offsets size:                              " << m_map_of_request_type_offsets.size()  ;
        DR_LOG(log_debug) << "m_mapDialogs size:                                               " << m_mapDialogs.size()  ;
        DR_LOG(log_debug) << "m_mapNetTransactions size:                                       " << m_mapNetTransactions.size()  ;
        DR_LOG(log_debug) << "m_mapAppTransactions size:                                       " << m_mapAppTransactions.size()  ;
        DR_LOG(log_debug) << "m_mapApiRequests size:                                           " << m_mapApiRequests.size()  ;
        DR_LOG(log_debug) << "m_mapDialogId2Appname size:                                      " << m_mapDialogId2Appname.size()  ;


    }
    boost::shared_ptr<SipDialogController> ClientController::getDialogController(void) {
        return m_pController->getDialogController();
    }

    void ClientController::stop() {
        m_acceptor.cancel() ;
        m_ioservice.stop() ;
        m_thread.join() ;
    }

 }
